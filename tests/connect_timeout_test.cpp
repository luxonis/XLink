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

extern "C" int tcpipPlatformServer(const char* devPathRead, const char* devPathWrite, void** fd);

namespace {

constexpr int kDefaultNumConnections = 4;
constexpr int kDefaultBasePort = 11880;
constexpr int kDefaultConnectTimeoutMs = 500;
constexpr int kDefaultServerHoldMs = 5000;

}  // namespace

int main(int argc, char** argv) {
    const int numConnections = testutils::parseIntArg(argc, argv, 1, kDefaultNumConnections);
    const int basePort = testutils::parseIntArg(argc, argv, 2, kDefaultBasePort);
    const int connectTimeoutMs = testutils::parseIntArg(argc, argv, 3, kDefaultConnectTimeoutMs);
    const int serverHoldMs = testutils::parseIntArg(argc, argv, 4, kDefaultServerHoldMs);

    XLinkGlobalHandler_t globalHandler = {};
    mvLogDefaultLevelSet(MVLOG_ERROR);
    if (XLinkInitialize(&globalHandler) != X_LINK_SUCCESS) {
        return -1;
    }

    std::vector<std::thread> serverThreads;
    std::vector<std::thread> clientThreads;
    std::atomic<bool> success{true};
    serverThreads.reserve(numConnections);
    clientThreads.reserve(numConnections);

    for (int i = 0; i < numConnections; ++i) {
        const std::string endpoint = testutils::makeEndpoint(basePort + i);
        serverThreads.emplace_back([endpoint, serverHoldMs]() {
            void* fd = nullptr;
            tcpipPlatformServer(endpoint.c_str(), endpoint.c_str(), &fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(serverHoldMs));
        });
    }

    testutils::sleepBriefly();
    for (int i = 0; i < numConnections; ++i) {
        clientThreads.emplace_back([&, i]() {
            std::string endpoint = testutils::makeEndpoint(basePort + i);
            XLinkHandler_t handler = testutils::makeTcpHandler(endpoint);
            if (XLinkConnectTimeout(&handler, connectTimeoutMs) != X_LINK_TIMEOUT) {
                success.store(false);
            }
        });
    }

    for (auto& thread : clientThreads) {
        thread.join();
    }
    for (auto& thread : serverThreads) {
        thread.join();
    }
    return success.load() ? 0 : -1;
}
