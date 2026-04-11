#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

#include "TestUtils.hpp"

namespace {

constexpr int kDefaultNumConnections = 8;
constexpr int kDefaultBasePort = 11520;
constexpr int kDefaultTimeoutMs = 5000;

std::atomic<int>* gLinkDownCount = nullptr;

void onLinkDown(linkId_t) {
    if (gLinkDownCount != nullptr) {
        gLinkDownCount->fetch_add(1);
    }
}

int runServer(const std::string& endpoint) {
    std::string serverEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(serverEndpoint);
    if (XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
        return -1;
    }

    const auto stream = XLinkOpenStream(handler.linkId, "tmp", 1024);
    if (stream == INVALID_STREAM_ID) {
        return -1;
    }

    uint8_t data[1024] = {};
    return XLinkWriteData(stream, data, sizeof(data)) == X_LINK_SUCCESS ? 0 : -1;
}

int runClient(const std::string& endpoint) {
    std::string clientEndpoint = endpoint;
    XLinkHandler_t handler = testutils::makeTcpHandler(clientEndpoint);
    if (!testutils::connectWithRetry(&handler, kDefaultTimeoutMs)) {
        return -1;
    }

    const auto stream = XLinkOpenStream(handler.linkId, "tmp", 1024);
    if (stream == INVALID_STREAM_ID) {
        return -1;
    }

    streamPacketDesc_t* packet = nullptr;
    if (XLinkReadData(stream, &packet) != X_LINK_SUCCESS) {
        return -1;
    }

    XLinkReleaseData(stream);
    return XLinkResetRemote(handler.linkId) == X_LINK_SUCCESS ? 0 : -1;
}

}  // namespace

int main(int argc, char** argv) {
    const int numConnections = testutils::parseIntArg(argc, argv, 1, kDefaultNumConnections);
    const int basePort = testutils::parseIntArg(argc, argv, 2, kDefaultBasePort);
    const int timeoutMs = testutils::parseIntArg(argc, argv, 3, kDefaultTimeoutMs);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    testutils::ProcessWatchdog watchdog(timeoutMs, "connection_shutdown_test");

    std::atomic<int> linkDownCount{0};
    gLinkDownCount = &linkDownCount;
    const int callbackId = XLinkAddLinkDownCb(onLinkDown);
    if (callbackId < 0) {
        return -1;
    }

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
        serverThreads.emplace_back([&, i]() { serverResults[i] = runServer(endpoints[i]); });
    }
    testutils::sleepBriefly();
    for (int i = 0; i < numConnections; ++i) {
        clientThreads.emplace_back([&, i]() { clientResults[i] = runClient(endpoints[i]); });
    }

    for (auto& thread : clientThreads) {
        thread.join();
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (linkDownCount.load() < numConnections && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& thread : serverThreads) {
        thread.join();
    }

    XLinkRemoveLinkDownCb(callbackId);
    gLinkDownCount = nullptr;

    if (linkDownCount.load() < numConnections) {
        return -1;
    }
    for (int result : serverResults) {
        if (result != 0) {
            return -1;
        }
    }
    for (int result : clientResults) {
        if (result != 0) {
            return -1;
        }
    }
    return 0;
}
