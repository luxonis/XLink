#include "tcpip_host.h"
#include "../src/pc/PlatformDeviceFd.h"

#include <linux/errqueue.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void waitFor(Predicate ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!ready() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(ready(), "Timed out waiting for retained payload state");
}

enum class Scenario { PartialWrites, FaultAfterPrefix, StallAfterPrefix };
struct Socket {
    explicit Socket(Scenario scenario) : scenario(scenario) {
        int pair[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "Cannot allocate test socket");
        fd = pair[0];
        peer = pair[1];
        key = createPlatformDeviceFdKey(reinterpret_cast<void*>(static_cast<uintptr_t>(fd)));
    }
    ~Socket() { close(peer); }

    void complete(uint32_t first, uint32_t last, bool copied = false) {
        std::lock_guard<std::mutex> lock(mutex);
        enqueue(first, last, copied);
    }

    void enqueue(uint32_t first, uint32_t last, bool copied) {
        sock_extended_err completion{};
        completion.ee_origin = SO_EE_ORIGIN_ZEROCOPY;
        completion.ee_info = first;
        completion.ee_data = last;
        if (copied) completion.ee_code = SO_EE_CODE_ZEROCOPY_COPIED;
        completions.push_back(completion);
        changed.notify_all();
    }

    XLinkRetainedSendStats_t stats() {
        XLinkRetainedSendStats_t result{};
        require(tcpipPlatformGetRetainedSendStats(key, &result) == X_LINK_SUCCESS, "Cannot read retained send stats");
        return result;
    }

    const Scenario scenario;
    int fd;
    int peer;
    void* key;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> bytes;
    std::deque<sock_extended_err> completions;
    unsigned accepted = 0;
    bool aborted = false;
};

Socket* active = nullptr;

struct Payload {
    Payload(Socket& socket, uint8_t value, size_t size) : bytes(size, value) {
        uint8_t trailer[] = {0xa3, 0x21};
        event.header.type = XLINK_WRITE_REQ;
        event.header.size = size + sizeof(trailer);
        event.deviceHandle.protocol = X_LINK_TCP_IP;
        event.deviceHandle.xLinkFD = socket.key;
        event.data = bytes.data();
        event.data2Size = sizeof(trailer);
        event.payloadLease = XLinkPayloadLeaseCreate(&released, [](void* owner) {
            ++*static_cast<std::atomic<unsigned>*>(owner);
        }, trailer, sizeof(trailer));
        require(event.payloadLease, "Cannot allocate test lease");
        event.data2 = const_cast<uint8_t*>(XLinkPayloadLeaseTrailer(event.payloadLease));
        trailer[0] = 0;
    }

    void release() {
        XLinkPayloadLeaseRelease(event.payloadLease);
        event.payloadLease = nullptr;
    }

    void checkWire(const std::vector<uint8_t>& wire, size_t offset) {
        xLinkEventHeader_t header{};
        std::memcpy(&header, wire.data() + offset, sizeof(header));
        require(header.type == XLINK_WRITE_REQ && header.size == bytes.size() + 2, "Retained send changed the packet header");
        const auto* payload = wire.data() + offset + sizeof(header);
        require(std::memcmp(payload, bytes.data(), bytes.size()) == 0, "Partial send corrupted or duplicated payload bytes");
        require(payload[bytes.size()] == 0xa3 && payload[bytes.size() + 1] == 0x21, "Retained send changed the trailer");
    }

    std::atomic<unsigned> released{0};
    std::vector<uint8_t> bytes;
    xLinkEvent_t event{};
};

void releasesByCompletionRange() {
    Socket socket(Scenario::PartialWrites);
    active = &socket;
    Payload first(socket, 0x6b, 8);
    Payload second(socket, 0x85, 4);
    require(tcpipPlatformWriteRetained(&first.event) == 0, "Cannot send first payload");
    first.release();
    require(tcpipPlatformWriteRetained(&second.event) == 0, "Cannot send second payload");
    second.release();

    require(socket.bytes.size() == 2 * sizeof(xLinkEventHeader_t) + 16, "Wrong wire size");
    first.checkWire(socket.bytes, 0);
    second.checkWire(socket.bytes, sizeof(xLinkEventHeader_t) + 10);
    require(first.released == 0 && second.released == 0, "A payload was released before its completions");

    socket.complete(1, 2, true);
    waitFor([&] { return second.released == 1; });
    require(first.released == 0, "A coalesced completion released an unrelated earlier send");
    socket.complete(UINT32_MAX, 0);
    waitFor([&] { return first.released == 1; });

    const auto stats = socket.stats();
    require(stats.zeroCopyBytes == 12 && stats.fallbackBytes == 0 && stats.submittedSends == 3 &&
            stats.completedSends == 3 && stats.copiedCompletions == 2, "Wrong completion or copy counters");
    require(tcpipPlatformClose(socket.key) == 0, "Could not close completed connection");
    active = nullptr;
}

void resumesAfterUnsupportedSend() {
    Socket socket(Scenario::FaultAfterPrefix);
    active = &socket;
    Payload payload(socket, 0x12, 12);
    require(tcpipPlatformWriteRetained(&payload.event) == 0, "Cannot fall back after a partial send");
    payload.release();

    require(socket.bytes.size() == sizeof(xLinkEventHeader_t) + 14, "Wrong wire size after fallback");
    payload.checkWire(socket.bytes, 0);
    require(payload.released == 0, "Fallback released a prefix still owned by the kernel");
    const auto stats = socket.stats();
    require(stats.zeroCopyBytes == 4 && stats.fallbackBytes == 8, "Fallback lost or resent the accepted prefix");
    socket.complete(0, 0);
    waitFor([&] { return payload.released == 1; });
    require(tcpipPlatformClose(socket.key) == 0, "Could not close fallback connection");
    active = nullptr;
}

void abortsWithAnOutstandingSend() {
    Socket socket(Scenario::StallAfterPrefix);
    active = &socket;
    Payload payload(socket, 0x34, 12);
    int writeResult = 0;
    std::thread writer([&] {
        writeResult = tcpipPlatformWriteRetained(&payload.event);
        payload.release();
    });

    bool accepted = false;
    try {
        waitFor([&] { return socket.stats().submittedSends == 1; });
        accepted = payload.released == 0;
    } catch (const std::exception&) {}
    const auto closeResult = tcpipPlatformClose(socket.key);
    writer.join();
    require(accepted, "Stalled writer did not retain its accepted prefix");
    require(closeResult == 0 && socket.aborted && writeResult != 0, "Disconnect did not abort the stalled writer");
    require(payload.released == 1, "Disconnect failed to drain the pending payload lease exactly once");
    active = nullptr;
}
}

extern "C" int __real_setsockopt(int, int, int, const void*, socklen_t);
extern "C" int __wrap_setsockopt(int fd, int level, int opt, const void* value, socklen_t size) {
    if (active && fd == active->fd && level == SOL_SOCKET && opt == SO_ZEROCOPY) return 0;
    return __real_setsockopt(fd, level, opt, value, size);
}

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if (!active || fd != active->fd) return __real_send(fd, data, size, flags);
    std::lock_guard<std::mutex> lock(active->mutex);
    if (flags & MSG_ZEROCOPY) {
        if (active->accepted && active->scenario != Scenario::PartialWrites) {
            errno = active->scenario == Scenario::StallAfterPrefix ? EAGAIN : EFAULT;
            return -1;
        }
        ++active->accepted;
        if (size > 4) size = 4;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    active->bytes.insert(active->bytes.end(), bytes, bytes + size);
    return size;
}

extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (!active || count != 1 || fds->fd != active->fd) return __real_poll(fds, count, timeout);
    std::unique_lock<std::mutex> lock(active->mutex);
    active->changed.wait_for(lock, std::chrono::milliseconds(timeout), [] {
        return !active->completions.empty() || active->aborted;
    });
    return !active->completions.empty() || active->aborted;
}

extern "C" ssize_t __real_recvmsg(int, msghdr*, int);
extern "C" ssize_t __wrap_recvmsg(int fd, msghdr* message, int flags) {
    if (!active || fd != active->fd || !(flags & MSG_ERRQUEUE)) return __real_recvmsg(fd, message, flags);
    std::lock_guard<std::mutex> lock(active->mutex);
    if (active->completions.empty()) {
        errno = EAGAIN;
        return -1;
    }
    auto* header = CMSG_FIRSTHDR(message);
    header->cmsg_level = SOL_IP;
    header->cmsg_type = IP_RECVERR;
    header->cmsg_len = CMSG_LEN(sizeof(sock_extended_err));
    std::memcpy(CMSG_DATA(header), &active->completions.front(), sizeof(sock_extended_err));
    message->msg_controllen = CMSG_SPACE(sizeof(sock_extended_err));
    active->completions.pop_front();
    return 0;
}

extern "C" int __real_connect(int, const sockaddr*, socklen_t);
extern "C" int __wrap_connect(int fd, const sockaddr* address, socklen_t size) {
    if (!active || fd != active->fd || address->sa_family != AF_UNSPEC) return __real_connect(fd, address, size);
    std::lock_guard<std::mutex> lock(active->mutex);
    active->aborted = true;
    active->enqueue(0, active->accepted - 1, false);
    return 0;
}

int main() {
    try {
        releasesByCompletionRange();
        resumesAfterUnsupportedSend();
        abortsWithAnOutstandingSend();
        std::puts("Retained TCP partial sends, ordered wire bytes, fallback, completion ranges and abort passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        std::_Exit(1);
    }
}
