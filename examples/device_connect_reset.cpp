#include <XLink/XLink.h>
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <array>

int main(int argc, char** argv) {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    if(argc <= 1) {
        printf("Usage: %s [usb/ip]\n", argv[0]);
        return 1;
    }

    // Search for booted device
    deviceDesc_t deviceDesc;
    strncpy(deviceDesc.name, argv[1], sizeof(deviceDesc.name));
    deviceDesc.nameHintOnly = false;

    std::string name(argv[1]);
    XLinkProtocol_t protocol = X_LINK_USB_VSC;
    for(const char& c : name) {
        if(c == '.') {
            protocol = X_LINK_TCP_IP;
        }
    }

    printf("Connecting to device name: %s, protocol: %s\n", deviceDesc.name, XLinkProtocolToStr(protocol));

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = protocol;
    if(XLinkConnect(&handler) != X_LINK_SUCCESS) {
        printf("Couldn't connect to the device\n");
        return -1;
    }

    // Wait 1s
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Reset the device
    XLinkResetRemote(handler.linkId);

    return 0;
}