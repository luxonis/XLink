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
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    // Get all flash booted devices
    unsigned int numdev = 0;
    std::array<deviceDesc_t, 32> deviceDescAll = {};
    deviceDesc_t suitableDevice = {};
    suitableDevice.protocol = X_LINK_ANY_PROTOCOL;
    suitableDevice.platform = X_LINK_ANY_PLATFORM;
    suitableDevice.state = X_LINK_UNBOOTED;

    status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
    if(status != X_LINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

    if(numdev == 0){
        std::cout << "No " << XLinkDeviceStateToStr(X_LINK_UNBOOTED) << " devices found to boot" << std::endl;
        return 0;
    }

    for(unsigned i = 0; i < numdev; i++) {
        std::cout << "Booting: " << deviceDescAll.at(i).name << " with: " << argv[1] << std::endl;
        XLinkBoot(&deviceDescAll.at(i), argv[1]);
    }

}