#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

namespace {

constexpr std::size_t kPayloadSize = 4096;
constexpr int kConnectRetryMs = 5;
constexpr int kDefaultConnectTimeoutMs = 5000;
constexpr int kDefaultWarmupRounds = 200;
constexpr int kDefaultSampleEveryRounds = 100;
constexpr std::size_t kDefaultRssGrowthLimitKiB = 24 * 1024;
constexpr int kDefaultFdGrowthLimit = 16;
constexpr int kDefaultServerGraceMs = 250;
constexpr char kStreamName[] = "resource_cleanup";

struct LeakTestConfig {
    int connectTimeoutMs = kDefaultConnectTimeoutMs;
    int warmupRounds = kDefaultWarmupRounds;
    int sampleEveryRounds = kDefaultSampleEveryRounds;
    std::size_t rssGrowthLimitKiB = kDefaultRssGrowthLimitKiB;
    int fdGrowthLimit = kDefaultFdGrowthLimit;
    int serverGraceMs = kDefaultServerGraceMs;
};

struct ResourceSnapshot {
    std::size_t rssKiB = 0;
    int openFds = -1;
};

int readEnvInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

LeakTestConfig readConfig() {
    LeakTestConfig cfg;
    cfg.connectTimeoutMs = readEnvInt("XLINK_LEAK_TEST_CONNECT_TIMEOUT_MS", kDefaultConnectTimeoutMs);
    cfg.warmupRounds = readEnvInt("XLINK_LEAK_TEST_WARMUP_ROUNDS", kDefaultWarmupRounds);
    cfg.sampleEveryRounds = readEnvInt("XLINK_LEAK_TEST_SAMPLE_EVERY_ROUNDS", kDefaultSampleEveryRounds);
    cfg.rssGrowthLimitKiB = static_cast<std::size_t>(readEnvInt("XLINK_LEAK_TEST_RSS_GROWTH_LIMIT_KIB",
        static_cast<int>(kDefaultRssGrowthLimitKiB)));
    cfg.fdGrowthLimit = readEnvInt("XLINK_LEAK_TEST_FD_GROWTH_LIMIT", kDefaultFdGrowthLimit);
    cfg.serverGraceMs = readEnvInt("XLINK_LEAK_TEST_SERVER_GRACE_MS", kDefaultServerGraceMs);
    return cfg;
}

std::size_t getCurrentRssKiB() {
#ifdef __APPLE__
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
#ifdef __APPLE__
    const char* fdDir = "/dev/fd";
#else
    const char* fdDir = "/proc/self/fd";
#endif

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

}  // namespace

#ifdef XLINK_TEST_CLIENT

int main(int argc, char** argv) {
    const LeakTestConfig cfg = readConfig();

    if (argc != 3) {
        std::printf("Usage: %s [num_rounds] [ip:port]\n", argv[0]);
        return -1;
    }

    const int numRounds = std::atoi(argv[1]);
    if (numRounds <= 0) {
        std::printf("Invalid num_rounds: %s\n", argv[1]);
        return -1;
    }

    XLinkGlobalHandler_t gHandler = {};
    if (XLinkInitialize(&gHandler) != X_LINK_SUCCESS) {
        std::printf("Failed to initialize XLink\n");
        return -1;
    }

    std::string devicePath{argv[2]};
    std::vector<std::uint8_t> payload(kPayloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    const int baselineRound = numRounds > cfg.warmupRounds ? cfg.warmupRounds : 1;
    ResourceSnapshot baseline = {};
    ResourceSnapshot maxObserved = {};
    bool baselineSet = false;

    for (int round = 0; round < numRounds; ++round) {
        XLinkHandler_t handler = {};
        handler.devicePath = &devicePath[0];
        handler.protocol = X_LINK_TCP_IP;

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
            std::printf("FAIL: round=%d connect failed (%s)\n", round, XLinkErrorToStr(connectStatus));
            return -1;
        }

        const streamId_t stream = XLinkOpenStream(&handler, kStreamName, static_cast<int>(payload.size() * 2));
        if (stream == INVALID_STREAM_ID) {
            std::printf("FAIL: round=%d open stream failed\n", round);
            return -1;
        }

        const XLinkError_t writeStatus = XLinkWriteData(&handler, stream, payload.data(), static_cast<int>(payload.size()));
        if (writeStatus != X_LINK_SUCCESS) {
            std::printf("FAIL: round=%d write failed (%s)\n", round, XLinkErrorToStr(writeStatus));
            return -1;
        }

        const XLinkError_t resetStatus = XLinkResetRemote(&handler);
        if (resetStatus != X_LINK_SUCCESS) {
            std::printf("FAIL: round=%d reset failed (%s)\n", round, XLinkErrorToStr(resetStatus));
            return -1;
        }

        const bool shouldSample = (round + 1) == baselineRound ||
            ((round + 1) > baselineRound && ((round + 1 - baselineRound) % cfg.sampleEveryRounds == 0)) ||
            round + 1 == numRounds;
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
        baselineSet = true;
    }

    const long long rssGrowthKiB = static_cast<long long>(maxObserved.rssKiB) -
        static_cast<long long>(baseline.rssKiB);
    const int fdGrowth = (baseline.openFds >= 0 && maxObserved.openFds >= 0) ?
        (maxObserved.openFds - baseline.openFds) : 0;

    printSnapshot("SUMMARY_BASELINE", baselineRound, baseline);
    printSnapshot("SUMMARY_MAX", numRounds, maxObserved, &baseline);

    if (rssGrowthKiB > static_cast<long long>(cfg.rssGrowthLimitKiB)) {
        std::printf("FAIL: rss growth exceeded limit: growth_kib=%lld limit_kib=%zu\n",
            rssGrowthKiB, cfg.rssGrowthLimitKiB);
        return -1;
    }

    if (fdGrowth > cfg.fdGrowthLimit) {
        std::printf("FAIL: open fd growth exceeded limit: growth=%d limit=%d\n",
            fdGrowth, cfg.fdGrowthLimit);
        return -1;
    }

    std::printf("Success! rounds=%d rss_growth_kib=%lld fd_growth=%d\n",
        numRounds, rssGrowthKiB, fdGrowth);
    return 0;
}

#endif

#ifdef XLINK_TEST_SERVER

namespace {

struct LinkDownState {
    std::mutex mutex;
    std::condition_variable cv;
    bool linkDown = false;
};

LinkDownState& getLinkDownState() {
    static auto* state = new LinkDownState();
    return *state;
}

}  // namespace

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::printf("Usage: %s [ip:port]\n", argv[0]);
        return -1;
    }

    const LeakTestConfig cfg = readConfig();

    XLinkGlobalHandler_t gHandler = {};
    gHandler.protocol = X_LINK_TCP_IP;
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&gHandler) != X_LINK_SUCCESS) {
        std::printf("Failed to initialize XLink\n");
        return -1;
    }

    auto& linkDownState = getLinkDownState();
    {
        std::lock_guard<std::mutex> lock(linkDownState.mutex);
        linkDownState.linkDown = false;
    }

    const int cbId = XLinkAddLinkDownCb([](linkId_t) {
        auto& state = getLinkDownState();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.linkDown = true;
        }
        state.cv.notify_all();
    });
    if (cbId < 0) {
        std::printf("Failed to register link-down callback\n");
        return -1;
    }

    XLinkHandler_t handler = {};
    std::string serverIp{argv[1]};
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;

    if (XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
        std::printf("Server failed to start\n");
        return -1;
    }

    {
        std::unique_lock<std::mutex> lock(linkDownState.mutex);
        if (!linkDownState.linkDown) {
            linkDownState.cv.wait_for(lock, std::chrono::milliseconds(cfg.serverGraceMs), []() {
                return getLinkDownState().linkDown;
            });
        }
    }

    XLinkRemoveLinkDownCb(cbId);

    return 0;
}

#endif
