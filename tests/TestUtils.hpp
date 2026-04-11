#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"

namespace testutils {

inline int parseIntArg(int argc, char** argv, int index, int defaultValue) {
    if (index >= argc) {
        return defaultValue;
    }
    return std::atoi(argv[index]);
}

inline std::size_t parseSizeArg(int argc, char** argv, int index, std::size_t defaultValue) {
    if (index >= argc) {
        return defaultValue;
    }
    const long long value = std::atoll(argv[index]);
    return value > 0 ? static_cast<std::size_t>(value) : defaultValue;
}

inline std::string makeEndpoint(int port) {
    return "127.0.0.1:" + std::to_string(port);
}

inline XLinkHandler_t makeTcpHandler(std::string& endpoint) {
    XLinkHandler_t handler = {};
    handler.devicePath = &endpoint[0];
    handler.protocol = X_LINK_TCP_IP;
    return handler;
}

inline bool connectWithRetry(XLinkHandler_t* handler, int timeoutMs, int retryMs = 5) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if (XLinkConnect(handler) == X_LINK_SUCCESS) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

inline std::vector<int> shuffledIndices(int count, unsigned seed) {
    std::vector<int> indices;
    indices.reserve(count);
    for (int i = 0; i < count; ++i) {
        indices.push_back(i);
    }

    std::mt19937 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);
    return indices;
}

inline void sleepBriefly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

inline void printUsageAndExit(const char* argv0, const char* usage) {
    std::fprintf(stderr, "Usage: %s %s\n", argv0, usage);
    std::exit(1);
}

class ProcessWatchdog {
public:
    explicit ProcessWatchdog(int timeoutMs, const char* testName = "test")
        : testName_(testName), worker_([this, timeoutMs]() {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool completed = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
                return done_.load();
            });
            if (!completed) {
                std::fprintf(stderr, "%s timed out after %d ms\n", testName_, timeoutMs);
                std::_Exit(2);
            }
        }) {}

    ~ProcessWatchdog() {
        finish();
    }

    ProcessWatchdog(const ProcessWatchdog&) = delete;
    ProcessWatchdog& operator=(const ProcessWatchdog&) = delete;

    void finish() {
        if (!worker_.joinable()) {
            return;
        }
        done_.store(true);
        cv_.notify_one();
        worker_.join();
    }

private:
    const char* testName_;
    std::atomic<bool> done_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};

}  // namespace testutils
