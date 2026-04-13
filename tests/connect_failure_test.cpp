#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
constexpr TestSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
constexpr TestSocket kInvalidSocket = -1;
#endif

namespace {

constexpr int kDefaultRounds = 1000;
constexpr int kDefaultBasePort = 11930;
constexpr int kDefaultWatchdogMs = 90000;
constexpr std::int32_t kXLinkPingReq = 5;
constexpr std::int32_t kXLinkPingResp = 12;

struct WireEventHeader {
    std::int32_t id;
    std::int32_t type;
    char streamName[MAX_STREAM_NAME_LENGTH];
    std::uint32_t tnsec;
    std::uint32_t tsecLsb;
    std::uint32_t tsecMsb;
    streamId_t streamId;
    std::uint32_t size;
    std::uint32_t flagsRaw;
};

enum class ServerMode {
    none,
    acceptOnly,
    pingOnly,
    regular,
};

struct CaseConfig {
    const char* name;
    ServerMode mode;
    int portOffset;
    int connectTimeoutMs;
    int serverDelayMs;
    XLinkError_t expectedStatus;
};

struct LinkDownState {
    std::mutex mutex;
    std::condition_variable cv;
    bool linkDown = false;
};

const CaseConfig kCases[] = {
    {"no_server", ServerMode::none, 0, 5, 0, X_LINK_DEVICE_NOT_FOUND},
    // Current behavior: a bare TCP accept is enough for XLinkConnectTimeout() to succeed.
    {"accept_only", ServerMode::acceptOnly, 1, 20, 0, X_LINK_TIMEOUT},
    {"ping_only_timeout", ServerMode::pingOnly, 2, 10, 30, X_LINK_TIMEOUT},
    {"ping_only_success", ServerMode::pingOnly, 3, 100, 0, X_LINK_SUCCESS},
    {"regular_server", ServerMode::regular, 4, 100, 0, X_LINK_SUCCESS},
};

void closeSocket(TestSocket socketFd) {
    if (socketFd == kInvalidSocket) {
        return;
    }
#if defined(_WIN32) || defined(_WIN64)
    closesocket(socketFd);
#else
    close(socketFd);
#endif
}

bool setReuseAddr(TestSocket socketFd) {
    const int reuseAddr = 1;
#if defined(_WIN32) || defined(_WIN64)
    const auto optLen = static_cast<int>(sizeof(reuseAddr));
#else
    const auto optLen = static_cast<socklen_t>(sizeof(reuseAddr));
#endif
    return setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&reuseAddr),
                      optLen) == 0;
}

bool bindAndListen(TestSocket* listenFd, const std::string& endpoint) {
    if (listenFd == nullptr) {
        return false;
    }

    TestSocket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        std::perror("socket");
        return false;
    }

#if defined(SO_NOSIGPIPE)
    const int set = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

    if (!setReuseAddr(fd)) {
        std::perror("setsockopt(SO_REUSEADDR)");
        closeSocket(fd);
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::stoi(endpoint.substr(endpoint.find(':') + 1))));
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        std::fprintf(stderr, "inet_pton failed for endpoint=%s\n", endpoint.c_str());
        closeSocket(fd);
        return false;
    }

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        closeSocket(fd);
        return false;
    }

    if (listen(fd, 1) != 0) {
        std::perror("listen");
        closeSocket(fd);
        return false;
    }

    *listenFd = fd;
    return true;
}

bool recvAll(TestSocket socketFd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t received = 0;
    while (received < size) {
#if defined(_WIN32) || defined(_WIN64)
        const int rc = recv(socketFd, reinterpret_cast<char*>(bytes + received), static_cast<int>(size - received), 0);
#else
        const ssize_t rc = recv(socketFd, bytes + received, size - received, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(rc);
    }
    return true;
}

bool sendAll(TestSocket socketFd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    while (sent < size) {
#if defined(_WIN32) || defined(_WIN64)
        const int rc = send(socketFd, reinterpret_cast<const char*>(bytes + sent), static_cast<int>(size - sent), 0);
#else
        const ssize_t rc = send(socketFd, bytes + sent, size - sent, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

class RawServer {
public:
    RawServer(ServerMode mode, std::string endpoint, int delayMs)
        : mode_(mode), endpoint_(std::move(endpoint)), delayMs_(delayMs), worker_([this]() { run(); }) {}

    ~RawServer() {
        join();
    }

    RawServer(const RawServer&) = delete;
    RawServer& operator=(const RawServer&) = delete;

    bool waitUntilReady(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool signaled = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() { return ready_; });
        return signaled && startOk_;
    }

    int result() const {
        return result_;
    }

    void requestStop() {
        stopRequested_.store(true);
        cv_.notify_all();
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        TestSocket listenFd = kInvalidSocket;
        if (!bindAndListen(&listenFd, endpoint_)) {
            result_ = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                startOk_ = false;
                ready_ = true;
            }
            cv_.notify_all();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            startOk_ = true;
            ready_ = true;
        }
        cv_.notify_all();

        struct sockaddr_in client = {};
#if defined(_WIN32) || defined(_WIN64)
        int clientLen = sizeof(client);
#else
        socklen_t clientLen = sizeof(client);
#endif
        const TestSocket connFd = accept(listenFd, reinterpret_cast<struct sockaddr*>(&client), &clientLen);
        closeSocket(listenFd);
        if (connFd == kInvalidSocket) {
            result_ = -1;
            return;
        }

        if (mode_ == ServerMode::acceptOnly) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(1000), [&]() { return stopRequested_.load(); });
            lock.unlock();
            closeSocket(connFd);
            result_ = 0;
            return;
        }

        WireEventHeader header = {};
        if (!recvAll(connFd, &header, sizeof(header))) {
            closeSocket(connFd);
            result_ = -1;
            return;
        }

        if (header.type != kXLinkPingReq) {
            closeSocket(connFd);
            result_ = -1;
            return;
        }

        if (delayMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        }

        header.type = kXLinkPingResp;
        header.flagsRaw = 1u;
        (void)sendAll(connFd, &header, sizeof(header));
        closeSocket(connFd);
        result_ = 0;
    }

    ServerMode mode_;
    std::string endpoint_;
    int delayMs_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_ = false;
    bool startOk_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<int> result_{-1};
};

class RegularServer {
public:
    explicit RegularServer(std::string endpoint) : endpoint_(std::move(endpoint)), worker_([this]() { run(); }) {}

    ~RegularServer() {
        join();
    }

    RegularServer(const RegularServer&) = delete;
    RegularServer& operator=(const RegularServer&) = delete;

    bool waitUntilReady(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!started_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return started_.load();
    }

    int result() const {
        return result_.load();
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    static void onLinkDown(void* context) {
        auto* state = static_cast<LinkDownState*>(context);
        if (state == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->linkDown = true;
        }
        state->cv.notify_all();
    }

    void run() {
        std::string endpoint = endpoint_;
        XLinkHandler_t handler = testutils::makeTcpHandler(endpoint);
        handler.linkDownCallback = &RegularServer::onLinkDown;
        handler.linkDownCallbackContext = &linkDownState_;
        started_.store(true);

        if (XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
            result_.store(-1);
            return;
        }

        std::unique_lock<std::mutex> lock(linkDownState_.mutex);
        if (!linkDownState_.linkDown) {
            linkDownState_.cv.wait_for(lock, std::chrono::milliseconds(1000), [&]() { return linkDownState_.linkDown; });
        }
        lock.unlock();

        if (!linkDownState_.linkDown) {
            result_.store(-1);
            return;
        }

        const XLinkError_t cleanupStatus = XLinkResetRemote(&handler);
        if (cleanupStatus != X_LINK_SUCCESS && cleanupStatus != X_LINK_COMMUNICATION_NOT_OPEN) {
            result_.store(-1);
            return;
        }

        result_.store(0);
    }

    std::string endpoint_;
    std::thread worker_;
    std::atomic<bool> started_{false};
    std::atomic<int> result_{-1};
    LinkDownState linkDownState_;
};

bool validateFailedHandler(const XLinkHandler_t& handler) {
    return handler.session == nullptr && handler.linkId == INVALID_LINK_ID;
}

int runCase(const CaseConfig& cfg, int port) {
    const std::string endpoint = testutils::makeEndpoint(port);

    std::unique_ptr<RawServer> rawServer;
    std::unique_ptr<RegularServer> regularServer;
    bool rawServerJoined = false;

    if (cfg.mode == ServerMode::acceptOnly || cfg.mode == ServerMode::pingOnly) {
        rawServer.reset(new RawServer(cfg.mode, endpoint, cfg.serverDelayMs));
        if (!rawServer->waitUntilReady(1000)) {
            return -1;
        }
    } else if (cfg.mode == ServerMode::regular) {
        regularServer.reset(new RegularServer(endpoint));
        if (!regularServer->waitUntilReady(1000)) {
            return -1;
        }
        testutils::sleepBriefly();
    }

    std::string clientEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(clientEndpoint);
    const XLinkError_t status = XLinkConnectTimeout(&handler, cfg.connectTimeoutMs);

    if (status != cfg.expectedStatus) {
        std::fprintf(stderr,
                     "case=%s endpoint=%s expected=%d actual=%d timeout_ms=%d delay_ms=%d\n",
                     cfg.name,
                     endpoint.c_str(),
                     static_cast<int>(cfg.expectedStatus),
                     static_cast<int>(status),
                     cfg.connectTimeoutMs,
                     cfg.serverDelayMs);
        if (rawServer) {
            rawServer->requestStop();
            rawServer->join();
            rawServerJoined = true;
        }
        if (regularServer) {
            regularServer->join();
        }
        return -1;
    }

    if (status == X_LINK_SUCCESS) {
        if (rawServer) {
            rawServer->requestStop();
            rawServer->join();
            rawServerJoined = true;
            if (rawServer->result() != 0) {
                return -1;
            }
        }

        const XLinkError_t cleanupStatus = XLinkResetRemote(&handler);
        if (cleanupStatus != X_LINK_SUCCESS && cleanupStatus != X_LINK_COMMUNICATION_NOT_OPEN) {
            if (rawServer) {
                rawServerJoined = true;
            }
            if (regularServer) {
                regularServer->join();
            }
            return -1;
        }
    } else if (!validateFailedHandler(handler)) {
        std::fprintf(stderr,
                     "case=%s endpoint=%s failed connect left handler dirty: session=%p linkId=%d\n",
                     cfg.name,
                     endpoint.c_str(),
                     static_cast<void*>(handler.session),
                     handler.linkId);
        if (rawServer) {
            rawServer->requestStop();
        }
        if (rawServer) {
            rawServer->join();
        }
        if (regularServer) {
            regularServer->join();
        }
        return -1;
    }

    if (rawServer) {
        if (!rawServerJoined) {
            rawServer->requestStop();
            rawServer->join();
        }
        if (rawServer->result() != 0) {
            return -1;
        }
    }
    if (regularServer) {
        regularServer->join();
        if (regularServer->result() != 0) {
            return -1;
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = testutils::parseIntArg(argc, argv, 1, kDefaultRounds);
    const int basePort = testutils::parseIntArg(argc, argv, 2, kDefaultBasePort);
    const int watchdogMs = testutils::parseIntArg(argc, argv, 3, kDefaultWatchdogMs);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    testutils::ProcessWatchdog watchdog(watchdogMs, "connect_failure_test");

    for (int round = 0; round < rounds; ++round) {
        for (const auto& cfg : kCases) {
            const int port = basePort + cfg.portOffset;
            if (runCase(cfg, port) != 0) {
                std::fprintf(stderr, "connect_failure_test failed on round=%d case=%s port=%d\n",
                             round, cfg.name, port);
                return -1;
            }
        }
    }

    return 0;
}
