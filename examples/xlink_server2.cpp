#include <string>
#include <vector>
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){
    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;


    printf("Initializing XLink...\n");
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        return 0;
    }


    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    status = XLinkServer(&handler, "xlinkserver", X_LINK_BOOTED, X_LINK_KEEMBAY);
    if(X_LINK_SUCCESS != status) {
        return 0;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1000));

    return 0;
}