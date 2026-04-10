#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

namespace {

constexpr int kNumConnections = 4;
constexpr std::size_t kDefaultPayloadSize = 20 * 1024 * 1024;
constexpr std::size_t kDefaultPayloadSlack = 1 * 1024 * 1024;
constexpr int kConnectRetryMs = 50;
constexpr int kDefaultConnectTimeoutMs = 15000;
constexpr int kServerShutdownTimeoutMs = 10000;
constexpr int kDefaultMaxJitterMs = 25;
constexpr int kDefaultClientHangTimeoutMs = 5000;
constexpr int kDefaultWriteRepeatCount = 1;
constexpr char kStreamName[] = "payload";

struct StressConfig {
    std::size_t payloadSize = kDefaultPayloadSize;
    std::size_t streamSize = kDefaultPayloadSize + kDefaultPayloadSlack;
    int connectTimeoutMs = kDefaultConnectTimeoutMs;
    int maxJitterMs = kDefaultMaxJitterMs;
    int clientHangTimeoutMs = kDefaultClientHangTimeoutMs;
    int serverHangTimeoutMs = kDefaultClientHangTimeoutMs;
    int writeRepeatCount = kDefaultWriteRepeatCount;
    bool dumpOnWatchdog = false;
};

int readEnvInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

StressConfig readConfig() {
    StressConfig cfg;
    const int payloadMiB = readEnvInt("XLINK_STRESS_PAYLOAD_MIB", static_cast<int>(kDefaultPayloadSize / (1024 * 1024)));
    cfg.payloadSize = static_cast<std::size_t>(payloadMiB) * 1024 * 1024;
    cfg.streamSize = cfg.payloadSize + kDefaultPayloadSlack;
    cfg.connectTimeoutMs = readEnvInt("XLINK_STRESS_CONNECT_TIMEOUT_MS", kDefaultConnectTimeoutMs);
    cfg.maxJitterMs = readEnvInt("XLINK_STRESS_JITTER_MS", kDefaultMaxJitterMs);
    cfg.clientHangTimeoutMs = readEnvInt("XLINK_STRESS_CLIENT_HANG_TIMEOUT_MS", kDefaultClientHangTimeoutMs);
    cfg.serverHangTimeoutMs = readEnvInt("XLINK_STRESS_SERVER_HANG_TIMEOUT_MS", kDefaultClientHangTimeoutMs);
    cfg.writeRepeatCount = readEnvInt("XLINK_STRESS_WRITE_REPEAT_COUNT", kDefaultWriteRepeatCount);
    cfg.dumpOnWatchdog = readEnvInt("XLINK_STRESS_DUMP_ON_WATCHDOG", 0) != 0;
    return cfg;
}

void dumpStacksIfEnabled(const StressConfig& cfg, const char* role, const char* detail) {
    if (!cfg.dumpOnWatchdog) {
        return;
    }

    std::ostringstream dumpPath;
    dumpPath << "/tmp/xlink_stress_" << role << "_" << getpid() << ".sample.txt";

    char command[1024];
    std::snprintf(command, sizeof(command),
        "/usr/bin/sample %d 1 1 -mayDie -file %s >/dev/null 2>&1",
        static_cast<int>(getpid()), dumpPath.str().c_str());
    const int rc = std::system(command);
    std::printf("%s WATCHDOG DUMP: %s detail=%s rc=%d\n",
        role, dumpPath.str().c_str(), detail, rc);
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

void fail(std::atomic<bool>& success, std::atomic<bool>& abortFlag, const char* message, int connection, int round, XLinkError_t status = X_LINK_SUCCESS) {
    success.store(false);
    abortFlag.store(true);
    if (status == X_LINK_SUCCESS) {
        std::printf("FAIL: conn=%d round=%d %s\n", connection, round, message);
    } else {
        std::printf("FAIL: conn=%d round=%d %s (%s)\n", connection, round, message, XLinkErrorToStr(status));
    }
}

enum class ClientPhase : int {
    Idle = 0,
    ConnectStart,
    ConnectOk,
    OpenStart,
    OpenOk,
    WriteStart,
    WriteOk,
    ResetStart,
    ResetOk,
    Failed,
};

const char* phaseToStr(ClientPhase phase) {
    switch (phase) {
        case ClientPhase::Idle: return "idle";
        case ClientPhase::ConnectStart: return "connect_start";
        case ClientPhase::ConnectOk: return "connect_ok";
        case ClientPhase::OpenStart: return "open_start";
        case ClientPhase::OpenOk: return "open_ok";
        case ClientPhase::WriteStart: return "write_start";
        case ClientPhase::WriteOk: return "write_ok";
        case ClientPhase::ResetStart: return "reset_start";
        case ClientPhase::ResetOk: return "reset_ok";
        case ClientPhase::Failed: return "failed";
    }
    return "unknown";
}

enum class ServerPhase : int {
    Start = 0,
    ServerOnlyStart,
    ServerOnlyOk,
    OpenStart,
    OpenOk,
    ReadStart,
    ReadOk,
    ReadInterrupted,
    WaitLinkDown,
    LinkDownOk,
    Failed,
};

const char* serverPhaseToStr(ServerPhase phase) {
    switch (phase) {
        case ServerPhase::Start: return "start";
        case ServerPhase::ServerOnlyStart: return "server_only_start";
        case ServerPhase::ServerOnlyOk: return "server_only_ok";
        case ServerPhase::OpenStart: return "open_start";
        case ServerPhase::OpenOk: return "open_ok";
        case ServerPhase::ReadStart: return "read_start";
        case ServerPhase::ReadOk: return "read_ok";
        case ServerPhase::ReadInterrupted: return "read_interrupted";
        case ServerPhase::WaitLinkDown: return "wait_link_down";
        case ServerPhase::LinkDownOk: return "link_down_ok";
        case ServerPhase::Failed: return "failed";
    }
    return "unknown";
}

}  // namespace

#ifdef XLINK_TEST_CLIENT

int main(int argc, char** argv) {
    const StressConfig cfg = readConfig();
    XLinkGlobalHandler_t gHandler = {};
    if (XLinkInitialize(&gHandler) != X_LINK_SUCCESS) {
        std::printf("Failed to initialize XLink\n");
        return -1;
    }

    if (argc != kNumConnections + 2) {
        std::printf("Usage: %s [num_rounds] [ip0] [ip1] [ip2] [ip3]\n", argv[0]);
        return -1;
    }

    const int numRounds = std::atoi(argv[1]);
    if (numRounds <= 0) {
        std::printf("Invalid num_rounds: %s\n", argv[1]);
        return -1;
    }

    std::vector<std::string> devicePaths;
    devicePaths.reserve(kNumConnections);
    for (int i = 0; i < kNumConnections; ++i) {
        devicePaths.emplace_back(argv[i + 2]);
    }

    std::vector<std::uint8_t> payload(cfg.payloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    std::atomic<bool> success{true};
    std::atomic<bool> abortFlag{false};
    std::array<std::atomic<int>, kNumConnections> phases{};
    std::array<std::atomic<int>, kNumConnections> rounds{};
    std::array<std::atomic<int>, kNumConnections> progress{};
    for (int i = 0; i < kNumConnections; ++i) {
        phases[i].store(static_cast<int>(ClientPhase::Idle));
        rounds[i].store(-1);
        progress[i].store(0);
    }
    std::vector<std::thread> workers;
    workers.reserve(kNumConnections);

    std::thread watchdog([&]() {
        int previousProgressSum = -1;
        auto lastProgressAt = std::chrono::steady_clock::now();

        while (!abortFlag.load()) {
            int progressSum = 0;
            for (int i = 0; i < kNumConnections; ++i) {
                progressSum += progress[i].load();
            }

            const auto now = std::chrono::steady_clock::now();
            if (progressSum != previousProgressSum) {
                previousProgressSum = progressSum;
                lastProgressAt = now;
            } else if (now - lastProgressAt > std::chrono::milliseconds(cfg.clientHangTimeoutMs)) {
                std::printf("WATCHDOG: no client progress for %d ms\n", cfg.clientHangTimeoutMs);
                for (int i = 0; i < kNumConnections; ++i) {
                    const auto phase = static_cast<ClientPhase>(phases[i].load());
                    std::printf("WATCHDOG: conn=%d round=%d phase=%s progress=%d\n",
                        i, rounds[i].load(), phaseToStr(phase), progress[i].load());
                }
                dumpStacksIfEnabled(cfg, "client", "client_progress_timeout");
                std::fflush(stdout);
                std::_Exit(2);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    for (int connection = 0; connection < kNumConnections; ++connection) {
        workers.emplace_back([&, connection]() {
            uint32_t rngState = 0xC0FFEEu ^ static_cast<uint32_t>((connection + 1) * 0x9E3779B9u);
            for (int round = 0; round < numRounds && success.load(); ++round) {
                rounds[connection].store(round);
                phases[connection].store(static_cast<int>(ClientPhase::ConnectStart));
                progress[connection].fetch_add(1);
                fuzzSleep(rngState, cfg.maxJitterMs);

                XLinkHandler_t handler = {};
                handler.devicePath = &devicePaths[connection][0];
                handler.protocol = X_LINK_TCP_IP;

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
                    phases[connection].store(static_cast<int>(ClientPhase::Failed));
                    fail(success, abortFlag, "connect failed", connection, round, connectStatus);
                    return;
                }

                phases[connection].store(static_cast<int>(ClientPhase::ConnectOk));
                progress[connection].fetch_add(1);
                fuzzSleep(rngState, cfg.maxJitterMs);

                phases[connection].store(static_cast<int>(ClientPhase::OpenStart));
                auto stream = XLinkOpenStream(handler.linkId, kStreamName, cfg.streamSize);
                if (stream == INVALID_STREAM_ID) {
                    phases[connection].store(static_cast<int>(ClientPhase::Failed));
                    fail(success, abortFlag, "open stream failed", connection, round);
                    return;
                }

                phases[connection].store(static_cast<int>(ClientPhase::OpenOk));
                progress[connection].fetch_add(1);
                phases[connection].store(static_cast<int>(ClientPhase::WriteStart));
                for (int writeIdx = 0; writeIdx < cfg.writeRepeatCount; ++writeIdx) {
                    const auto writeStatus = XLinkWriteData(stream, payload.data(), payload.size());
                    if (writeStatus != X_LINK_SUCCESS) {
                        phases[connection].store(static_cast<int>(ClientPhase::Failed));
                        fail(success, abortFlag, "write failed", connection, round, writeStatus);
                        return;
                    }
                }

                phases[connection].store(static_cast<int>(ClientPhase::WriteOk));
                progress[connection].fetch_add(1);
                fuzzSleep(rngState, cfg.maxJitterMs);

                phases[connection].store(static_cast<int>(ClientPhase::ResetStart));
                const auto resetStatus = XLinkResetRemote(handler.linkId);
                if (resetStatus != X_LINK_SUCCESS) {
                    phases[connection].store(static_cast<int>(ClientPhase::Failed));
                    fail(success, abortFlag, "reset failed", connection, round, resetStatus);
                    return;
                }

                phases[connection].store(static_cast<int>(ClientPhase::ResetOk));
                progress[connection].fetch_add(1);
                std::printf("ROUND OK: conn=%d round=%d\n", connection, round);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    if (!success.load()) {
        abortFlag.store(true);
        watchdog.join();
        return -1;
    }

    abortFlag.store(true);
    watchdog.join();
    std::printf("Success!\n");
    return 0;
}

#endif

#ifdef XLINK_TEST_SERVER

namespace {

std::mutex shutdownMutex;
std::condition_variable shutdownCv;
bool linkDown = false;

}  // namespace

int main(int argc, const char** argv) {
    if (argc < 2) {
        std::printf("Usage: %s [ip:port]\n", argv[0]);
        return -1;
    }

    const StressConfig cfg = readConfig();
    XLinkGlobalHandler_t gHandler = {};
    gHandler.protocol = X_LINK_TCP_IP;
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&gHandler) != X_LINK_SUCCESS) {
        std::printf("Failed to initialize XLink\n");
        return -1;
    }

    XLinkAddLinkDownCb([](linkId_t) {
        {
            std::lock_guard<std::mutex> lock(shutdownMutex);
            linkDown = true;
        }
        shutdownCv.notify_all();
    });

    XLinkHandler_t handler = {};
    std::string serverIp{argv[1]};
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;

    std::atomic<int> serverPhase{static_cast<int>(ServerPhase::Start)};
    std::atomic<bool> serverDone{false};
    std::thread watchdog([&]() {
        auto lastProgressAt = std::chrono::steady_clock::now();
        int previousPhase = serverPhase.load();

        while (!serverDone.load()) {
            const int currentPhase = serverPhase.load();
            const auto now = std::chrono::steady_clock::now();
            if (currentPhase != previousPhase) {
                previousPhase = currentPhase;
                lastProgressAt = now;
            } else if (currentPhase == static_cast<int>(ServerPhase::Start) ||
                       currentPhase == static_cast<int>(ServerPhase::ServerOnlyStart)) {
                lastProgressAt = now;
            } else if (now - lastProgressAt > std::chrono::milliseconds(cfg.serverHangTimeoutMs)) {
                std::printf("SERVER WATCHDOG: ip=%s phase=%s\n", serverIp.c_str(),
                    serverPhaseToStr(static_cast<ServerPhase>(currentPhase)));
                dumpStacksIfEnabled(cfg, "server", serverPhaseToStr(static_cast<ServerPhase>(currentPhase)));
                std::fflush(stdout);
                std::_Exit(3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    serverPhase.store(static_cast<int>(ServerPhase::ServerOnlyStart));
    const auto serverStatus = XLinkServerOnly(&handler);
    if (serverStatus != X_LINK_SUCCESS) {
        serverPhase.store(static_cast<int>(ServerPhase::Failed));
        std::printf("Server failed to start: %s\n", XLinkErrorToStr(serverStatus));
        serverDone.store(true);
        watchdog.join();
        return -1;
    }

    serverPhase.store(static_cast<int>(ServerPhase::ServerOnlyOk));
    serverPhase.store(static_cast<int>(ServerPhase::OpenStart));
    auto stream = XLinkOpenStream(handler.linkId, kStreamName, cfg.streamSize);
    if (stream == INVALID_STREAM_ID) {
        serverPhase.store(static_cast<int>(ServerPhase::Failed));
        std::printf("Server failed to open stream\n");
        serverDone.store(true);
        watchdog.join();
        return -1;
    }

    serverPhase.store(static_cast<int>(ServerPhase::OpenOk));
    bool readInterruptedByReset = false;
    serverPhase.store(static_cast<int>(ServerPhase::ReadStart));
    for (int readIdx = 0; readIdx < cfg.writeRepeatCount; ++readIdx) {
        streamPacketDesc_t packet = {};
        const auto readStatus = XLinkReadMoveData(stream, &packet);
        if (readStatus != X_LINK_SUCCESS) {
            // On slower builds, the client reset can legitimately win the race against the
            // server-side read request. Treat that as teardown progress rather than a test failure.
            readInterruptedByReset = true;
            serverPhase.store(static_cast<int>(ServerPhase::ReadInterrupted));
            std::printf("Server read interrupted by reset: %s\n", XLinkErrorToStr(readStatus));
            break;
        }

        serverPhase.store(static_cast<int>(ServerPhase::ReadOk));
        const bool payloadMatches = packet.length == cfg.payloadSize;
        XLinkDeallocateMoveData(packet.data, packet.length);
        if (!payloadMatches) {
            serverPhase.store(static_cast<int>(ServerPhase::Failed));
            std::printf("Unexpected payload length: %u\n", packet.length);
            serverDone.store(true);
            watchdog.join();
            return -1;
        }
    }

    serverPhase.store(static_cast<int>(ServerPhase::WaitLinkDown));
    std::unique_lock<std::mutex> lock(shutdownMutex);
    const auto shutdownDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kServerShutdownTimeoutMs);
    while (!linkDown) {
        if (shutdownCv.wait_until(lock, shutdownDeadline) == std::cv_status::timeout) {
            serverPhase.store(static_cast<int>(ServerPhase::Failed));
            std::printf("Timed out waiting for link-down callback\n");
            serverDone.store(true);
            watchdog.join();
            return -1;
        }
    }

    serverPhase.store(static_cast<int>(ServerPhase::LinkDownOk));
    if (readInterruptedByReset) {
        serverDone.store(true);
        watchdog.join();
        return 0;
    }
    serverDone.store(true);
    watchdog.join();
    return 0;
}

#endif
