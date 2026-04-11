#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

namespace {

constexpr int kDefaultNumConnections = 16;
constexpr int kDefaultNumStreams = 16;
constexpr int kDefaultNumPackets = 120;
constexpr int kDefaultBasePort = 11490;
constexpr int kDefaultTimeoutMs = 30000;
const uint8_t kDummyData[1024 * 128] = {};

int runClientConnection(const std::string& endpoint, int connectionId, int numStreams, int numPackets) {
    std::string clientEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(clientEndpoint);
    if (!testutils::connectWithRetry(&handler, 5000)) {
        return -1;
    }

    const auto order = testutils::shuffledIndices(numStreams, static_cast<unsigned>(connectionId + 1));
    std::vector<streamId_t> streams(numStreams, INVALID_STREAM_ID);
    std::vector<std::thread> threads;
    threads.reserve(numStreams);

    for (int index : order) {
        threads.emplace_back([&, index]() {
            const std::string name = "test_" + std::to_string(index);
            streams[index] = XLinkOpenStream(&handler, name.c_str(), sizeof(kDummyData) * 2);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto stream : streams) {
        if (stream == INVALID_STREAM_ID) {
            return -1;
        }
    }

    std::atomic<bool> success{true};
    threads.clear();
    for (int index : order) {
        threads.emplace_back([&, index]() {
            const auto stream = streams[index];
            for (int packet = 0; packet < numPackets; ++packet) {
                if (XLinkWriteData(&handler, stream, kDummyData, sizeof(kDummyData)) != X_LINK_SUCCESS) {
                    success.store(false);
                    return;
                }

                streamPacketDesc_t desc = {};
                const auto status = XLinkReadMoveData(&handler, stream, &desc);
                if (status != X_LINK_SUCCESS || desc.data == nullptr || desc.length != sizeof(uint32_t)) {
                    success.store(false);
                    return;
                }

                uint32_t echoed = UINT32_MAX;
                std::memcpy(&echoed, desc.data, sizeof(echoed));
                XLinkDeallocateMoveData(desc.data, desc.length);
                if (echoed != static_cast<uint32_t>(index)) {
                    success.store(false);
                    return;
                }
            }

            if (XLinkCloseStream(&handler, stream) != X_LINK_SUCCESS) {
                success.store(false);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    if (!success.load()) {
        return -1;
    }
    return XLinkResetRemote(&handler) == X_LINK_SUCCESS ? 0 : -1;
}

int runServerConnection(const std::string& endpoint, int numStreams, int numPackets) {
    std::string serverEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(serverEndpoint);
    printf("Server listening on %s, numStreams=%d, numPackets=%d\n", endpoint.c_str(), numStreams, numPackets);
    if (XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
        return -1;
    }

    std::vector<streamId_t> streams(numStreams, INVALID_STREAM_ID);
    std::vector<std::thread> writerThreads;
    std::vector<std::thread> readerThreads;
    writerThreads.reserve(numStreams);
    readerThreads.reserve(numStreams);

    for (int i = 0; i < numStreams; ++i) {
        const std::string name = "test_" + std::to_string(i);
        streams[i] = XLinkOpenStream(&handler, name.c_str(), sizeof(kDummyData) * 2);
        if (streams[i] == INVALID_STREAM_ID) {
            return -1;
        }

        writerThreads.emplace_back([&, i]() {
            const auto stream = streams[i];
            const uint32_t payload = static_cast<uint32_t>(i);
            for (int packet = 0; packet < numPackets; ++packet) {
                const auto status = XLinkWriteData2(&handler, stream,
                    reinterpret_cast<const uint8_t*>(&payload), sizeof(payload) / 2,
                    reinterpret_cast<const uint8_t*>(&payload) + sizeof(payload) / 2, sizeof(payload) - sizeof(payload) / 2);
                assert(status == X_LINK_SUCCESS);
            }
        });
        readerThreads.emplace_back([&, i]() {
            const auto stream = streams[i];
            for (int packet = 0; packet < numPackets; ++packet) {
                streamPacketDesc_t desc = {};
                const auto status = XLinkReadMoveData(&handler, stream, &desc);
                assert(status == X_LINK_SUCCESS);
                XLinkDeallocateMoveData(desc.data, desc.length);
            }
        });
    }

    for (auto& thread : writerThreads) {
        thread.join();
    }
    for (auto& thread : readerThreads) {
        thread.join();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int numConnections = testutils::parseIntArg(argc, argv, 1, kDefaultNumConnections);
    const int basePort = testutils::parseIntArg(argc, argv, 2, kDefaultBasePort);
    const int numStreams = testutils::parseIntArg(argc, argv, 3, kDefaultNumStreams);
    const int numPackets = testutils::parseIntArg(argc, argv, 4, kDefaultNumPackets);
    const int timeoutMs = testutils::parseIntArg(argc, argv, 5, kDefaultTimeoutMs);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    std::atomic<bool> done{false};
    std::mutex doneMutex;
    std::condition_variable doneCv;
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(doneMutex);
        const bool completed = doneCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            return done.load();
        });
        if (!completed) {
            printf("Test timed out after %d ms\n", timeoutMs);
            std::_Exit(2);
        }
    });

    std::vector<std::string> endpoints;
    endpoints.reserve(numConnections);
    for (int i = 0; i < numConnections; ++i) {
        endpoints.push_back(testutils::makeEndpoint(basePort + i));
    }

    std::vector<int> serverResults(numConnections, -1);
    std::vector<int> clientResults(numConnections, -1);
    std::vector<std::thread> serverThreads;
    std::vector<std::thread> clientThreads;
    serverThreads.reserve(numConnections);
    clientThreads.reserve(numConnections);

    for (int i = 0; i < numConnections; ++i) {
        serverThreads.emplace_back([&, i]() { serverResults[i] = runServerConnection(endpoints[i], numStreams, numPackets); });
    }
    testutils::sleepBriefly();
    for (int i = 0; i < numConnections; ++i) {
        clientThreads.emplace_back([&, i]() { clientResults[i] = runClientConnection(endpoints[i], i, numStreams, numPackets); });
    }

    for (auto& thread : clientThreads) {
        thread.join();
    }
    for (auto& thread : serverThreads) {
        thread.join();
    }

    for (int result : serverResults) {
        if (result != 0) {
            done.store(true);
            doneCv.notify_one();
            watchdog.join();
            return -1;
        }
    }
    for (int result : clientResults) {
        if (result != 0) {
            done.store(true);
            doneCv.notify_one();
            watchdog.join();
            return -1;
        }
    }
    done.store(true);
    doneCv.notify_one();
    watchdog.join();
    return 0;
}
