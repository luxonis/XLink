#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){

    if(argc < 2) {
        std::cout << "Usage: " << argv[0] << " path/to/cmd\n";
        return 0;
    }

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_LAST);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(XLINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    // Get all flash booted devices
    unsigned int numdev = 0;
    std::array<deviceDesc_t, 32> deviceDescAll = {};
    deviceDesc_t suitableDevice = {};
    suitableDevice.protocol = XLINK_ANY_PROTOCOL;
    suitableDevice.platform = XLINK_ANY_PLATFORM;
    suitableDevice.state = XLINK_UNBOOTED;

    status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
    if(status != XLINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

    if(numdev == 0){
        std::cout << "No " << XLinkDeviceStateToStr(XLINK_UNBOOTED) << " devices found to boot" << std::endl;
        return 0;
    }

    for(unsigned i = 0; i < numdev; i++) {
        std::cout << "Booting: " << deviceDescAll.at(i).name << " with: " << argv[1] << std::endl;
        XLinkBoot(&deviceDescAll.at(i), argv[1]);
    }

}