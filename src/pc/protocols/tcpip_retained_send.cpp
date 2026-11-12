#include "tcpip_host.h"
#include "tcpip_retained_send_support.h"
#include "../PlatformDeviceFd.h"

#define MVLOG_UNIT_NAME tcpip_host
#include "XLinkLog.h"

#ifdef __linux__
#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#endif

namespace {
int copyEvent(xLinkEvent_t* event) {
    void* key = event->deviceHandle.xLinkFD;
    const int payloadSize = static_cast<int>(event->header.size) - event->data2Size;
    if (tcpipPlatformWrite(key, &event->header, sizeof(event->header)) != 0 ||
        tcpipPlatformWrite(key, event->data, payloadSize) != 0) return -1;
    return event->data2Size ? tcpipPlatformWrite(key, event->data2, event->data2Size) : 0;
}
}

#ifdef __linux__
namespace {
std::mutex connectionMutex;

#if XLINK_LINUX_TCP_RETAINED_SEND
using Clock = std::chrono::steady_clock;
constexpr size_t maxSendBytes = 1024 * 1024;
constexpr size_t maxPinnedBytes = 4 * 1024 * 1024;
constexpr size_t maxPending = 64;
constexpr auto writeTimeout = std::chrono::seconds(5);
constexpr auto closeTimeout = std::chrono::seconds(1);
std::atomic<bool> canRetainMore{true};

struct PendingSend {
    uint32_t id;
    size_t charge;
    XLinkPayloadLease* owner;
};

class TcpRetainedSender {
public:
    explicit TcpRetainedSender(int socket) : socket(socket) {
        const long page = sysconf(_SC_PAGESIZE);
        if (page > 0) pinAllowance = 2 * static_cast<size_t>(page);
        int enable = 1;
        zeroCopy = canRetainMore.load(std::memory_order_relaxed) && pinAllowance < maxPinnedBytes &&
                   setsockopt(socket, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable)) == 0;
    }

    static void start(const std::shared_ptr<TcpRetainedSender>& sender) {
        sender->worker = std::thread([sender] { sender->reap(); });
    }

    int write(xLinkEvent_t* event) {
        std::unique_lock<std::mutex> lock(mutex);
        if (state != State::Open) return -1;
        auto deadline = Clock::now() + writeTimeout;
        if (!sendBytes(lock, reinterpret_cast<const uint8_t*>(&event->header), sizeof(event->header), nullptr, deadline) ||
            !sendBytes(lock, static_cast<const uint8_t*>(event->data), event->header.size - event->data2Size,
                       event->payloadLease, deadline)) {
            return -1;
        }
        return sendBytes(lock, static_cast<const uint8_t*>(event->data2), event->data2Size, nullptr, deadline) ? 0 : -1;
    }

    XLinkRetainedSendStats_t getStats() {
        std::lock_guard<std::mutex> lock(mutex);
        return stats;
    }

    int close() {
        std::unique_lock<std::mutex> lock(mutex);
        state = State::Closing;
        if (pendingCount) {
            sockaddr abortAddress{};
            abortAddress.sa_family = AF_UNSPEC;
            connect(socket, &abortAddress, sizeof(abortAddress));
        }
        shutdown(socket, SHUT_RDWR);
        changed.notify_all();
        const bool drained = changed.wait_for(lock, closeTimeout, [this] { return state == State::Closed; });
        if (!drained) {
            // Keep the socket open for completion notifications. Reconnects must not accumulate leases.
            canRetainMore.store(false, std::memory_order_relaxed);
            mvLog(MVLOG_ERROR, "Retained TCP socket did not drain %zu sends after abort. Disabled new zero-copy sends.",
                  pendingCount);
        }
        const auto totals = stats;
        const int result = drained ? closeResult : -1;
        lock.unlock();
        if (drained) worker.join();
        else worker.detach();

        mvLog(MVLOG_DEBUG,
              "Retained TCP payload: zeroCopyBytes=%llu fallbackBytes=%llu submittedSends=%llu completedSends=%llu copiedCompletions=%llu",
              static_cast<unsigned long long>(totals.zeroCopyBytes), static_cast<unsigned long long>(totals.fallbackBytes),
              static_cast<unsigned long long>(totals.submittedSends), static_cast<unsigned long long>(totals.completedSends),
              static_cast<unsigned long long>(totals.copiedCompletions));
        return result;
    }

private:
    bool sendBytes(std::unique_lock<std::mutex>& lock, const uint8_t* data, size_t length,
                   XLinkPayloadLease* owner, Clock::time_point& deadline) {
        while (length) {
            if (state != State::Open || Clock::now() >= deadline) return false;
            const bool retain = owner && zeroCopy && canRetainMore.load(std::memory_order_relaxed);
            size_t requested = length;
            if (retain) {
                if (pendingCount == pending.size() || pinnedBytes + pinAllowance >= maxPinnedBytes) {
                    changed.wait_until(lock, deadline);
                    continue;
                }
                requested = std::min({length, maxSendBytes, maxPinnedBytes - pinnedBytes - pinAllowance});
            }

            const auto count = send(socket, data, requested, MSG_DONTWAIT | MSG_NOSIGNAL | (retain ? MSG_ZEROCOPY : 0));
            if (count > 0) {
                if (retain) {
                    // Linux charges the requested range even when send only accepts a prefix.
                    const size_t charge = requested + pinAllowance;
                    XLinkPayloadLeaseRetain(owner);
                    pending[pendingCount++] = {nextId++, charge, owner};
                    pinnedBytes += charge;
                    stats.zeroCopyBytes += count;
                    ++stats.submittedSends;
                    changed.notify_all();
                } else if (owner) {
                    stats.fallbackBytes += count;
                }
                data += count;
                length -= static_cast<size_t>(count);
                deadline = Clock::now() + writeTimeout;
                continue;
            }
            if (count == 0) return false;

            const int error = errno;
            if (error == EINTR) continue;
            if (retain && error == ENOBUFS && pendingCount) {
                changed.wait_until(lock, deadline);
                continue;
            }
            if (retain && (error == ENOBUFS || error == EFAULT || error == EINVAL ||
                           error == EOPNOTSUPP || error == ENOSYS)) {
                zeroCopy = false;
                continue;
            }
            if (error != EAGAIN && error != EWOULDBLOCK) return false;
            lock.unlock();
            pollfd fd{socket, POLLOUT, 0};
            poll(&fd, 1, 20);
            lock.lock();
        }
        return true;
    }

    size_t complete(uint32_t first, uint32_t last, bool copied,
                    std::array<XLinkPayloadLease*, maxPending>& released, size_t count) {
        for (size_t i = 0; i < pendingCount;) {
            const auto item = pending[i];
            if (item.id - first > last - first) {
                ++i;
                continue;
            }
            released[count++] = item.owner;
            pinnedBytes -= item.charge;
            pending[i] = pending[--pendingCount];
            ++stats.completedSends;
            if (copied) ++stats.copiedCompletions;
        }
        return count;
    }

    void reap() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [this] { return state != State::Open || pendingCount; });
            if (state == State::Closing && pendingCount == 0) {
                closeResult = ::close(socket);
                state = State::Closed;
                changed.notify_all();
                return;
            }
            lock.unlock();
            pollfd fd{socket, 0, 0};
            poll(&fd, 1, 20);
            lock.lock();

            std::array<XLinkPayloadLease*, maxPending> released{};
            size_t count = 0;
            while (pendingCount) {
                alignas(cmsghdr) char control[CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_storage))]{};
                msghdr message{};
                message.msg_control = control;
                message.msg_controllen = sizeof(control);
                if (recvmsg(socket, &message, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
                    if ((header->cmsg_level != SOL_IP || header->cmsg_type != IP_RECVERR) &&
                        (header->cmsg_level != SOL_IPV6 || header->cmsg_type != IPV6_RECVERR)) continue;
                    if (header->cmsg_len < CMSG_LEN(sizeof(sock_extended_err))) continue;
                    sock_extended_err info{};
                    memcpy(&info, CMSG_DATA(header), sizeof(info));
                    if (info.ee_origin != SO_EE_ORIGIN_ZEROCOPY || info.ee_errno != 0) continue;
                    const bool copied = (info.ee_code & SO_EE_CODE_ZEROCOPY_COPIED) != 0;
                    count = complete(info.ee_info, info.ee_data, copied, released, count);
                    if (copied) zeroCopy = false;
                }
            }
            if (count) {
                changed.notify_all();
                lock.unlock();
                for (size_t i = 0; i < count; ++i) XLinkPayloadLeaseRelease(released[i]);
                lock.lock();
            } else {
                // POLLERR or POLLHUP can stay set before the last skb references are released.
                changed.wait_for(lock, std::chrono::milliseconds(20));
            }
        }
    }

    const int socket;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::array<PendingSend, maxPending> pending{};
    XLinkRetainedSendStats_t stats{};
    size_t pendingCount = 0;
    size_t pinnedBytes = 0;
    size_t pinAllowance = maxPinnedBytes;
    uint32_t nextId = 0;
    bool zeroCopy = false;
    enum class State { Open, Closing, Closed };
    State state = State::Open;
    int closeResult = 0;
};

std::unordered_map<void*, std::shared_ptr<TcpRetainedSender>> retainedConnections;
#endif
}

int tcpipCloseLinuxSocket(void* key) {
    void* raw = nullptr;
#if XLINK_LINUX_TCP_RETAINED_SEND
    std::shared_ptr<TcpRetainedSender> sender;
#endif
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        if (getPlatformDeviceFdFromKey(key, &raw) != 0 || destroyPlatformDeviceFdKey(key) != 0) return -1;
#if XLINK_LINUX_TCP_RETAINED_SEND
        const auto found = retainedConnections.find(key);
        if (found != retainedConnections.end()) {
            sender = found->second;
            retainedConnections.erase(found);
        }
#endif
    }
#if XLINK_LINUX_TCP_RETAINED_SEND
    if (sender) return sender->close();
#endif
    const int socket = static_cast<int>(reinterpret_cast<uintptr_t>(raw));
    shutdown(socket, SHUT_RDWR);
    return ::close(socket);
}
#endif

int tcpipPlatformWriteRetained(xLinkEvent_t* event) {
#if XLINK_LINUX_TCP_RETAINED_SEND
    std::shared_ptr<TcpRetainedSender> sender;
    try {
        std::lock_guard<std::mutex> lock(connectionMutex);
        void* key = event->deviceHandle.xLinkFD;
        auto found = retainedConnections.find(key);
        if (found != retainedConnections.end()) {
            sender = found->second;
        } else {
            void* raw = nullptr;
            if (getPlatformDeviceFdFromKey(key, &raw) != 0) return -1;
            sender = std::make_shared<TcpRetainedSender>(static_cast<int>(reinterpret_cast<uintptr_t>(raw)));
            retainedConnections.emplace(key, sender);
            try {
                TcpRetainedSender::start(sender);
            } catch (...) {
                retainedConnections.erase(key);
                throw;
            }
        }
    } catch (...) {
        return -1;
    }
    if (sender) return sender->write(event);
#endif
    return copyEvent(event);
}

XLinkError_t tcpipPlatformGetRetainedSendStats(void* key, XLinkRetainedSendStats_t* stats) {
    *stats = XLinkRetainedSendStats_t{};
#if XLINK_LINUX_TCP_RETAINED_SEND
    std::shared_ptr<TcpRetainedSender> sender;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        const auto found = retainedConnections.find(key);
        if (found != retainedConnections.end()) sender = found->second;
    }
    if (sender) *stats = sender->getStats();
    return X_LINK_SUCCESS;
#else
    (void)key;
    return X_LINK_NOT_IMPLEMENTED;
#endif
}
