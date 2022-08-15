#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(){

    mvLogDefaultLevelSet(MVLOG_WARN);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(XLINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkDeviceState_t state = XLINK_ANY_STATE;

    // Get all available devices
    unsigned int numdev = 0;
    std::array<deviceDesc_t, 32> deviceDescAll = {};
    deviceDesc_t suitableDevice = {};
    suitableDevice.protocol = XLINK_ANY_PROTOCOL;
    suitableDevice.platform = XLINK_ANY_PLATFORM;

    status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
    if(status != XLINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

    // Print device details
    for(int i = 0; i < numdev; i++){
        const auto& dev = deviceDescAll[i];
        std::cout << "status: " << XLinkErrorToStr(dev.status);
        std::cout << ", name: " << dev.name;
        std::cout << ", mxid: " << dev.mxid;
        std::cout << ", state: " << XLinkDeviceStateToStr(dev.state);
        std::cout << ", protocol: " << XLinkProtocolToStr(dev.protocol);
        std::cout << ", platform: " << XLinkPlatformToStr(dev.platform);
        std::cout << std::endl;
    }

}