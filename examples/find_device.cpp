#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>
#include <cstring>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, char* argv[]){

    mvLogDefaultLevelSet(MVLOG_WARN);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    deviceDesc_t desc, dev;
    desc.state = X_LINK_ANY_STATE;
    desc.platform = X_LINK_ANY_PLATFORM;
    desc.protocol = X_LINK_ANY_PROTOCOL;
    desc.mxid[0] = 0;

    if(argc >= 2) {
        strncpy(desc.name, argv[1], sizeof(desc.name));
        desc.nameHintOnly = true;
        printf("Name: %s\n", argv[1]);
    }
    if(argc >= 3) {
        strncpy(desc.mxid, argv[2], sizeof(desc.mxid));
        printf("ID: %s\n", argv[2]);
    }

    // Use "name" as hint only, but might still change
    status = XLinkFindFirstSuitableDevice(desc, &dev);
    if(status != X_LINK_SUCCESS) {
        printf("Couldnt find a device...\n");
        return -1;
    }

    std::cout << "status: " << XLinkErrorToStr(dev.status);
    std::cout << ", name: " << dev.name;
    std::cout << ", mxid: " << dev.mxid;
    std::cout << ", state: " << XLinkDeviceStateToStr(dev.state);
    std::cout << ", protocol: " << XLinkProtocolToStr(dev.protocol);
    std::cout << ", platform: " << XLinkPlatformToStr(dev.platform);
    std::cout << std::endl;

}