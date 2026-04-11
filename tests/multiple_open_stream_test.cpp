#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

namespace {

constexpr int kDefaultNumStreams = 16;
constexpr int kDefaultPort = 11480;
constexpr int kDefaultTimeoutMs = 10000;

int runServer(const std::string& endpoint, int numStreams) {
    std::string serverEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(serverEndpoint);
    if (XLinkServer(&handler, "multiple_open_stream_test", X_LINK_BOOTED, X_LINK_MYRIAD_X) != X_LINK_SUCCESS) {
        std::printf("Server failed to start\n");
        return -1;
    }

    std::vector<std::thread> threads;
    threads.reserve(numStreams);
    for (int i = 0; i < numStreams; ++i) {
        threads.emplace_back([&, i]() {
            const std::string name = "test_" + std::to_string(i);
            const auto stream = XLinkOpenStream(&handler, name.c_str(), 1024);
            assert(stream != INVALID_STREAM_ID);
            const uint32_t payload = static_cast<uint32_t>(i);
            const auto status = XLinkWriteData2(&handler, stream,
                reinterpret_cast<const uint8_t*>(&payload), sizeof(payload) / 2,
                reinterpret_cast<const uint8_t*>(&payload) + sizeof(payload) / 2, sizeof(payload) - sizeof(payload) / 2);
            assert(status == X_LINK_SUCCESS);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    return 0;
}

int runClient(const std::string& endpoint, int numStreams) {
    std::string clientEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(clientEndpoint);
    if (!testutils::connectWithRetry(&handler, 5000)) {
        std::printf("Client failed to connect\n");
        return -1;
    }

    const auto order = testutils::shuffledIndices(numStreams, 0x1234u);
    std::vector<streamId_t> streams(numStreams, INVALID_STREAM_ID);
    std::vector<std::thread> threads;
    threads.reserve(numStreams);

    for (int index : order) {
        threads.emplace_back([&, index]() {
            const std::string name = "test_" + std::to_string(index);
            streams[index] = XLinkOpenStream(&handler, name.c_str(), 1024);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto stream : streams) {
        if (stream == INVALID_STREAM_ID) {
            std::printf("Client failed to open one or more streams\n");
            return -1;
        }
    }

    threads.clear();
    std::atomic<bool> success{true};
    for (int index : order) {
        threads.emplace_back([&, index]() {
            streamPacketDesc_t* packet = nullptr;
            const auto stream = streams[index];
            const auto status = XLinkReadData(&handler, stream, &packet);
            if (status != X_LINK_SUCCESS || packet == nullptr || packet->length != sizeof(uint32_t)) {
                success.store(false);
                return;
            }

            uint32_t echoed = UINT32_MAX;
            std::memcpy(&echoed, packet->data, sizeof(echoed));
            if (echoed != static_cast<uint32_t>(index)) {
                success.store(false);
            }
            XLinkReleaseData(&handler, stream);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    if (XLinkResetRemote(&handler) != X_LINK_SUCCESS) {
        return -1;
    }
    return success.load() ? 0 : -1;
}

}  // namespace

int main(int argc, char** argv) {
    const int numStreams = testutils::parseIntArg(argc, argv, 1, kDefaultNumStreams);
    const int port = testutils::parseIntArg(argc, argv, 2, kDefaultPort);
    const int timeoutMs = testutils::parseIntArg(argc, argv, 3, kDefaultTimeoutMs);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        std::printf("Failed to initialize XLink\n");
        return -1;
    }

    testutils::ProcessWatchdog watchdog(timeoutMs, "multiple_open_stream_test");

    const std::string endpoint = testutils::makeEndpoint(port);
    int serverResult = -1;
    int clientResult = -1;

    std::thread server([&]() { serverResult = runServer(endpoint, numStreams); });
    testutils::sleepBriefly();
    std::thread client([&]() { clientResult = runClient(endpoint, numStreams); });

    client.join();
    server.join();

    if (serverResult != 0 || clientResult != 0) {
        std::printf("serverResult=%d clientResult=%d\n", serverResult, clientResult);
    }
    return (serverResult == 0 && clientResult == 0) ? 0 : -1;
}
