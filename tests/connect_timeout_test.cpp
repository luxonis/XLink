#include <cstdio>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <condition_variable>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

// Client
int main(int argc, char** argv) {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    int numConnections = 1;
    std::string localhost = "127.0.0.1";
    char* tmp[] = {nullptr, &localhost[0], nullptr};
    if(argc > 1) {
        numConnections = argc - 1;
    } else {
        argv = tmp;
    }
    std::vector<std::thread> connections;
    std::atomic<bool> allSuccess{true};
    for(int connection = 0; connection < numConnections; connection++) {
        connections.push_back(std::thread([connection, &allSuccess, argv](){

            deviceDesc_t deviceDesc;
            strcpy(deviceDesc.name, argv[connection+1]);
            deviceDesc.protocol = X_LINK_TCP_IP;

            printf("Device name: %s\n", deviceDesc.name);

            XLinkHandler_t handler;
            handler.devicePath = deviceDesc.name;
            handler.protocol = deviceDesc.protocol;
            auto connRet = XLinkConnectWithTimeout(&handler, 500);
            assert(connRet == X_LINK_TIMEOUT);
        }));

    }

    for(auto& conn : connections){
        conn.join();
    }

    if(allSuccess) {
        std::cout << "Success!\n";
        return 0;
    } else {
        std::cout << "RIP!\n";
        return -1;
    }
}

