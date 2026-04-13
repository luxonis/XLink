#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

namespace {

constexpr int kNumConnections = 4;
constexpr std::size_t kDefaultPayloadSize = 20 * 1024 * 1024;
constexpr std::size_t kDefaultPayloadSlack = 1 * 1024 * 1024;
constexpr int kConnectRetryMs = 50;
constexpr int kDefaultRounds = 64;
constexpr int kDefaultBasePort = 11690;
constexpr int kDefaultConnectTimeoutMs = 15000;
constexpr int kDefaultMaxJitterMs = 25;
constexpr int kDefaultClientHangTimeoutMs = 5000;
constexpr int kDefaultServerHangTimeoutMs = 5000;
constexpr int kDefaultWriteRepeatCount = 1;
constexpr int kDefaultTimeoutMs = 60000;
constexpr char kStreamName[] = "payload";

struct StressConfig {
    int rounds = kDefaultRounds;
    int basePort = kDefaultBasePort;
    std::size_t payloadSize = kDefaultPayloadSize;
    std::size_t streamSize = kDefaultPayloadSize + kDefaultPayloadSlack;
    int connectTimeoutMs = kDefaultConnectTimeoutMs;
    int maxJitterMs = kDefaultMaxJitterMs;
    int clientHangTimeoutMs = kDefaultClientHangTimeoutMs;
    int serverHangTimeoutMs = kDefaultServerHangTimeoutMs;
    int writeRepeatCount = kDefaultWriteRepeatCount;
    int timeoutMs = kDefaultTimeoutMs;
};

struct ServerState {
    std::mutex mutex;
    std::condition_variable cv;
    bool linkDown = false;
    std::atomic<int> round{-1};
    std::atomic<int> progress{0};
};

StressConfig parseConfig(int argc, char** argv) {
    StressConfig cfg;
    cfg.rounds = testutils::parseIntArg(argc, argv, 1, kDefaultRounds);
    cfg.basePort = testutils::parseIntArg(argc, argv, 2, kDefaultBasePort);
    const int payloadMiB = testutils::parseIntArg(argc, argv, 3, static_cast<int>(kDefaultPayloadSize / (1024 * 1024)));
    cfg.payloadSize = static_cast<std::size_t>(payloadMiB) * 1024 * 1024;
    cfg.streamSize = cfg.payloadSize + kDefaultPayloadSlack;
    cfg.maxJitterMs = testutils::parseIntArg(argc, argv, 4, kDefaultMaxJitterMs);
    cfg.writeRepeatCount = testutils::parseIntArg(argc, argv, 5, kDefaultWriteRepeatCount);
    cfg.connectTimeoutMs = testutils::parseIntArg(argc, argv, 6, kDefaultConnectTimeoutMs);
    cfg.clientHangTimeoutMs = testutils::parseIntArg(argc, argv, 7, kDefaultClientHangTimeoutMs);
    cfg.serverHangTimeoutMs = testutils::parseIntArg(argc, argv, 8, kDefaultServerHangTimeoutMs);
    cfg.timeoutMs = testutils::parseIntArg(argc, argv, 9, kDefaultTimeoutMs);
    return cfg;
}

uint32_t nextRand(uint32_t& state) {
    state = (state * 1664525u) + 1013904223u;
    return state;
}

void fuzzSleep(uint32_t& rngState, int maxSleepMs) {
    if (maxSleepMs <= 0) {
        return;
    }
    const auto delay = static_cast<int>(nextRand(rngState) % static_cast<uint32_t>(maxSleepMs + 1));
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    } else {
        std::this_thread::yield();
    }
}

void fail(std::atomic<bool>& success, const char* role, int connection, int round, const char* message, XLinkError_t status = X_LINK_SUCCESS) {
    success.store(false);
    if (status == X_LINK_SUCCESS) {
        std::printf("FAIL[%s]: conn=%d round=%d %s\n", role, connection, round, message);
    } else {
        std::printf("FAIL[%s]: conn=%d round=%d %s (%s)\n", role, connection, round, message, XLinkErrorToStr(status));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const StressConfig cfg = parseConfig(argc, argv);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    testutils::ProcessWatchdog watchdog(cfg.timeoutMs, "reset_reconnect_stress_test");

    std::vector<std::string> endpoints;
    endpoints.reserve(kNumConnections);
    for (int i = 0; i < kNumConnections; ++i) {
        endpoints.push_back(testutils::makeEndpoint(cfg.basePort + i));
    }

    std::vector<std::uint8_t> payload(cfg.payloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    std::atomic<bool> success{true};
    std::array<std::atomic<int>, kNumConnections> clientRounds{};
    std::array<std::atomic<int>, kNumConnections> clientProgress{};
    std::array<ServerState, kNumConnections> serverStates;
    for (int i = 0; i < kNumConnections; ++i) {
        clientRounds[i].store(-1);
        clientProgress[i].store(0);
    }

    std::vector<std::thread> serverThreads;
    serverThreads.reserve(kNumConnections);
    for (int connection = 0; connection < kNumConnections; ++connection) {
        serverThreads.emplace_back([&, connection]() {
            std::string endpoint = endpoints[connection];
            for (int round = 0; round < cfg.rounds && success.load(); ++round) {
                serverStates[connection].round.store(round);
                serverStates[connection].progress.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(serverStates[connection].mutex);
                    serverStates[connection].linkDown = false;
                }

                XLinkHandler_t handler = testutils::makeTcpHandler(endpoint);
                handler.linkDownCallback = [](void* context) {
                    auto* state = static_cast<ServerState*>(context);
                    if (state == nullptr) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->linkDown = true;
                    }
                    state->cv.notify_all();
                };
                handler.linkDownCallbackContext = &serverStates[connection];
                const auto serverStatus = XLinkServerOnly(&handler);
                if (serverStatus != X_LINK_SUCCESS) {
                    fail(success, "server", connection, round, "server start failed", serverStatus);
                    return;
                }
                serverStates[connection].progress.fetch_add(1);

                const auto stream = XLinkOpenStream(&handler, kStreamName, cfg.streamSize);
                if (stream == INVALID_STREAM_ID) {
                    fail(success, "server", connection, round, "open stream failed");
                    return;
                }
                serverStates[connection].progress.fetch_add(1);

                for (int writeIdx = 0; writeIdx < cfg.writeRepeatCount; ++writeIdx) {
                    streamPacketDesc_t packet = {};
                    const auto readStatus = XLinkReadMoveData(&handler, stream, &packet);
                    if (readStatus != X_LINK_SUCCESS) {
                        fail(success, "server", connection, round, "read interrupted", readStatus);
                        return;
                    }

                    const bool payloadMatches = packet.length == cfg.payloadSize;
                    XLinkDeallocateMoveData(packet.data, packet.length);
                    if (!payloadMatches) {
                        fail(success, "server", connection, round, "unexpected payload length");
                        return;
                    }
                }
                serverStates[connection].progress.fetch_add(1);

                std::unique_lock<std::mutex> lock(serverStates[connection].mutex);
                const bool linkDown = serverStates[connection].cv.wait_for(lock,
                    std::chrono::milliseconds(cfg.serverHangTimeoutMs),
                    [&]() { return serverStates[connection].linkDown; });
                if (!linkDown) {
                    fail(success, "server", connection, round, "timed out waiting for link-down");
                    return;
                }
                serverStates[connection].progress.fetch_add(1);
            }
        });
    }

    testutils::sleepBriefly();

    std::atomic<bool> clientWatchdogDone{false};
    std::thread clientWatchdog([&]() {
        int previousProgressSum = -1;
        auto lastProgressAt = std::chrono::steady_clock::now();

        while (!clientWatchdogDone.load()) {
            int progressSum = 0;
            for (int i = 0; i < kNumConnections; ++i) {
                progressSum += clientProgress[i].load();
            }

            const auto now = std::chrono::steady_clock::now();
            if (progressSum != previousProgressSum) {
                previousProgressSum = progressSum;
                lastProgressAt = now;
            } else if (now - lastProgressAt > std::chrono::milliseconds(cfg.clientHangTimeoutMs)) {
                success.store(false);
                clientWatchdogDone.store(true);
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::vector<std::thread> clientThreads;
    clientThreads.reserve(kNumConnections);
    for (int connection = 0; connection < kNumConnections; ++connection) {
        clientThreads.emplace_back([&, connection]() {
            uint32_t rngState = 0xC0FFEEu ^ static_cast<uint32_t>((connection + 1) * 0x9E3779B9u);
            std::string endpoint = endpoints[connection];

            for (int round = 0; round < cfg.rounds && success.load(); ++round) {
                clientRounds[connection].store(round);
                clientProgress[connection].fetch_add(1);
                fuzzSleep(rngState, cfg.maxJitterMs);

                XLinkHandler_t handler = testutils::makeTcpHandler(endpoint);
                XLinkError_t connectStatus = X_LINK_ERROR;
                const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.connectTimeoutMs);
                do {
                    connectStatus = XLinkConnect(&handler);
                    if (connectStatus == X_LINK_SUCCESS) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryMs));
                } while (std::chrono::steady_clock::now() < connectDeadline && success.load());

                if (connectStatus != X_LINK_SUCCESS) {
                    fail(success, "client", connection, round, "connect failed", connectStatus);
                    return;
                }
                clientProgress[connection].fetch_add(1);

                const auto stream = XLinkOpenStream(&handler, kStreamName, cfg.streamSize);
                if (stream == INVALID_STREAM_ID) {
                    fail(success, "client", connection, round, "open stream failed");
                    return;
                }
                clientProgress[connection].fetch_add(1);

                for (int writeIdx = 0; writeIdx < cfg.writeRepeatCount; ++writeIdx) {
                    const auto writeStatus = XLinkWriteData(&handler, stream, payload.data(), payload.size());
                    if (writeStatus != X_LINK_SUCCESS) {
                        fail(success, "client", connection, round, "write failed", writeStatus);
                        return;
                    }
                }
                clientProgress[connection].fetch_add(1);
                fuzzSleep(rngState, cfg.maxJitterMs);

                const auto resetStatus = XLinkResetRemote(&handler);
                if (resetStatus != X_LINK_SUCCESS) {
                    fail(success, "client", connection, round, "reset failed", resetStatus);
                    return;
                }
                clientProgress[connection].fetch_add(1);
            }
        });
    }

    for (auto& thread : clientThreads) {
        thread.join();
    }
    for (auto& thread : serverThreads) {
        thread.join();
    }

    clientWatchdogDone.store(true);
    clientWatchdog.join();
    return success.load() ? 0 : -1;
}
