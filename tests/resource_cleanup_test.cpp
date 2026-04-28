#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#else
#include <dirent.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

namespace {

constexpr std::size_t kPayloadSize = 4096;
constexpr int kConnectRetryMs = 5;
constexpr int kDefaultRounds = 500;
constexpr int kDefaultConnectTimeoutMs = 5000;
constexpr int kDefaultWarmupRounds = 50;
constexpr int kDefaultSampleEveryRounds = 50;
constexpr std::size_t kDefaultRssGrowthLimitKiB = 24 * 1024;
constexpr int kDefaultFdGrowthLimit = 16;
constexpr int kDefaultServerGraceMs = 250;
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kDefaultPort = 11780;
constexpr char kStreamName[] = "resource_cleanup";

struct LeakTestConfig {
    int rounds = kDefaultRounds;
    int connectTimeoutMs = kDefaultConnectTimeoutMs;
    int warmupRounds = kDefaultWarmupRounds;
    int sampleEveryRounds = kDefaultSampleEveryRounds;
    std::size_t rssGrowthLimitKiB = kDefaultRssGrowthLimitKiB;
    int fdGrowthLimit = kDefaultFdGrowthLimit;
    int serverGraceMs = kDefaultServerGraceMs;
    int timeoutMs = kDefaultTimeoutMs;
    int port = kDefaultPort;
};

struct ResourceSnapshot {
    std::size_t rssKiB = 0;
    int openFds = -1;
};

struct LinkDownState {
    std::mutex* mutex = nullptr;
    std::condition_variable* cv = nullptr;
    bool* flag = nullptr;
};

std::size_t getCurrentRssKiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX info = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info))) {
        return 0;
    }
    return static_cast<std::size_t>(info.WorkingSetSize / 1024);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info = {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(info.resident_size / 1024);
#elif defined(__linux__)
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }

    char line[256];
    std::size_t rssKiB = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::sscanf(line, "VmRSS: %zu kB", &rssKiB) == 1) {
            break;
        }
    }
    std::fclose(status);
    return rssKiB;
#else
    return 0;
#endif
}

int getOpenFdCount() {
#ifdef _WIN32
    DWORD handleCount = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
        return -1;
    }
    return static_cast<int>(handleCount);
#elif defined(__APPLE__)
    const char* fdDir = "/dev/fd";
#else
    const char* fdDir = "/proc/self/fd";
#endif

#ifndef _WIN32
    DIR* dir = opendir(fdDir);
    if (dir == nullptr) {
        return -1;
    }

    int count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);
    return count >= 2 ? count - 2 : count;
#endif
}

ResourceSnapshot captureResources() {
    ResourceSnapshot snapshot;
    snapshot.rssKiB = getCurrentRssKiB();
    snapshot.openFds = getOpenFdCount();
    return snapshot;
}

void printSnapshot(const char* label, int round, const ResourceSnapshot& snapshot,
        const ResourceSnapshot* baseline = nullptr) {
    long long rssDeltaKiB = 0;
    int fdDelta = 0;
    if (baseline != nullptr) {
        rssDeltaKiB = static_cast<long long>(snapshot.rssKiB) - static_cast<long long>(baseline->rssKiB);
        if (snapshot.openFds >= 0 && baseline->openFds >= 0) {
            fdDelta = snapshot.openFds - baseline->openFds;
        }
    }

    std::printf("%s: round=%d rss_kib=%zu open_fds=%d rss_delta_kib=%lld fd_delta=%d\n",
        label, round, snapshot.rssKiB, snapshot.openFds, rssDeltaKiB, fdDelta);
}

LeakTestConfig parseConfig(int argc, char** argv) {
    LeakTestConfig cfg;
    cfg.rounds = testutils::parseIntArg(argc, argv, 1, kDefaultRounds);
    cfg.port = testutils::parseIntArg(argc, argv, 2, kDefaultPort);
    cfg.connectTimeoutMs = testutils::parseIntArg(argc, argv, 3, kDefaultConnectTimeoutMs);
    cfg.warmupRounds = testutils::parseIntArg(argc, argv, 4, kDefaultWarmupRounds);
    cfg.sampleEveryRounds = testutils::parseIntArg(argc, argv, 5, kDefaultSampleEveryRounds);
    cfg.rssGrowthLimitKiB = testutils::parseSizeArg(argc, argv, 6, kDefaultRssGrowthLimitKiB);
    cfg.fdGrowthLimit = testutils::parseIntArg(argc, argv, 7, kDefaultFdGrowthLimit);
    cfg.serverGraceMs = testutils::parseIntArg(argc, argv, 8, kDefaultServerGraceMs);
    cfg.timeoutMs = testutils::parseIntArg(argc, argv, 9, kDefaultTimeoutMs);
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    const LeakTestConfig cfg = parseConfig(argc, argv);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    testutils::ProcessWatchdog watchdog(cfg.timeoutMs, "resource_cleanup_test");

    std::vector<std::uint8_t> payload(kPayloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    const std::string endpoint = testutils::makeEndpoint(cfg.port);

    const int baselineRound = cfg.rounds > cfg.warmupRounds ? cfg.warmupRounds : 1;
    ResourceSnapshot baseline = {};
    ResourceSnapshot maxObserved = {};
    bool baselineSet = false;

    for (int round = 0; round < cfg.rounds; ++round) {
        std::mutex linkDownMutex;
        std::condition_variable linkDownCv;
        bool linkDown = false;

        int serverResult = -1;
        std::thread server([&]() {
            std::string serverEndpoint = endpoint;
            XLinkHandler_t handler = testutils::makeTcpHandler(serverEndpoint);
            handler.linkDownCallback = [](XLinkLinkDownReason_t, void* context) {
                auto* state = static_cast<LinkDownState*>(context);
                if (state == nullptr || state->mutex == nullptr || state->cv == nullptr || state->flag == nullptr) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(*state->mutex);
                    *state->flag = true;
                }
                state->cv->notify_all();
            };
            LinkDownState callbackState;
            callbackState.mutex = &linkDownMutex;
            callbackState.cv = &linkDownCv;
            callbackState.flag = &linkDown;
            handler.linkDownCallbackContext = &callbackState;
            if (XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
                serverResult = -1;
                return;
            }

            std::unique_lock<std::mutex> lock(linkDownMutex);
            if (!linkDown) {
                linkDownCv.wait_for(lock, std::chrono::milliseconds(cfg.serverGraceMs), [&]() { return linkDown; });
            }
            lock.unlock();

            if (!linkDown) {
                serverResult = -1;
                return;
            }

            const XLinkError_t cleanupStatus = XLinkResetRemote(&handler);
            if (cleanupStatus != X_LINK_SUCCESS && cleanupStatus != X_LINK_COMMUNICATION_NOT_OPEN) {
                serverResult = -1;
                return;
            }

            serverResult = 0;
        });

        testutils::sleepBriefly();
        std::string clientEndpoint = endpoint;
        XLinkHandler_t handler = testutils::makeTcpHandler(clientEndpoint);

        XLinkError_t connectStatus = X_LINK_ERROR;
        const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.connectTimeoutMs);
        do {
            connectStatus = XLinkConnect(&handler);
            if (connectStatus == X_LINK_SUCCESS) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryMs));
        } while (std::chrono::steady_clock::now() < connectDeadline);

        if (connectStatus != X_LINK_SUCCESS) {
            server.join();
            return -1;
        }

        const auto stream = XLinkOpenStream(&handler, kStreamName, static_cast<int>(payload.size() * 2));
        if (stream == INVALID_STREAM_ID) {
            server.join();
            return -1;
        }

        if (XLinkWriteData(&handler, stream, payload.data(), static_cast<int>(payload.size())) != X_LINK_SUCCESS) {
            server.join();
            return -1;
        }

        if (XLinkResetRemote(&handler) != X_LINK_SUCCESS) {
            server.join();
            return -1;
        }

        server.join();
        if (serverResult != 0) {
            return -1;
        }

        const bool shouldSample = (round + 1) == baselineRound ||
            ((round + 1) > baselineRound && ((round + 1 - baselineRound) % cfg.sampleEveryRounds == 0)) ||
            round + 1 == cfg.rounds;
        if (!shouldSample) {
            continue;
        }

        const ResourceSnapshot snapshot = captureResources();
        if (!baselineSet) {
            baseline = snapshot;
            maxObserved = snapshot;
            baselineSet = true;
            printSnapshot("BASELINE", round + 1, snapshot);
            continue;
        }

        if (snapshot.rssKiB > maxObserved.rssKiB) {
            maxObserved.rssKiB = snapshot.rssKiB;
        }
        if (snapshot.openFds > maxObserved.openFds) {
            maxObserved.openFds = snapshot.openFds;
        }
        printSnapshot("RESOURCE", round + 1, snapshot, &baseline);
    }

    if (!baselineSet) {
        baseline = captureResources();
        maxObserved = baseline;
    }

    const long long rssGrowthKiB = static_cast<long long>(maxObserved.rssKiB) -
        static_cast<long long>(baseline.rssKiB);
    const int fdGrowth = (baseline.openFds >= 0 && maxObserved.openFds >= 0) ?
        (maxObserved.openFds - baseline.openFds) : 0;

    printSnapshot("SUMMARY_BASELINE", baselineRound, baseline);
    printSnapshot("SUMMARY_MAX", cfg.rounds, maxObserved, &baseline);

    if (rssGrowthKiB > static_cast<long long>(cfg.rssGrowthLimitKiB)) {
        return -1;
    }
    if (fdGrowth > cfg.fdGrowthLimit) {
        return -1;
    }
    return 0;
}
