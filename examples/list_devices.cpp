#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

struct DeviceInfo {
    deviceDesc_t desc;
    XLinkDeviceState_t state;
};

int main(){

    mvLogDefaultLevelSet(MVLOG_WARN);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    std::vector<DeviceInfo> devices;

    XLinkDeviceState_t state = X_LINK_ANY_STATE;

    std::vector<XLinkDeviceState_t> states;
    if(state == X_LINK_ANY_STATE) {
        states = {X_LINK_UNBOOTED, X_LINK_BOOTLOADER, X_LINK_BOOTED, X_LINK_FLASH_BOOTED};
    } else {
        states = {state};
    }

    // Get all available devices
    for(const auto& state : states) {
        unsigned int numdev = 0;
        std::array<deviceDesc_t, 32> deviceDescAll = {};
        deviceDesc_t suitableDevice = {};
        suitableDevice.protocol = X_LINK_ANY_PROTOCOL;
        suitableDevice.platform = X_LINK_ANY_PLATFORM;

        auto status = XLinkFindAllSuitableDevices(state, suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
        if(status != X_LINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

        for(unsigned i = 0; i < numdev; i++) {
            DeviceInfo info = {};
            info.desc = deviceDescAll.at(i);
            info.state = state;
            devices.push_back(info);
        }
    }

    // Print device details
    for(const auto& dev : devices){
        std::cout << "name: " << dev.desc.name;
        std::cout << ", state: " << XLinkDeviceStateToStr(dev.state);
        std::cout << ", protocol: " << XLinkProtocolToStr(dev.desc.protocol);
        std::cout << ", platform: " << XLinkPlatformToStr(dev.desc.platform);
        std::cout << std::endl;
    }

}