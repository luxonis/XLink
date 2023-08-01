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

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

// Common constants
const uint8_t DUMMY_DATA[1024*128] = {};
XLinkGlobalHandler_t xlinkGlobalHandler = {};

// Server
int main(int argc, const char** argv){
    xlinkGlobalHandler.protocol = X_LINK_USB_EP;

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_ERROR);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
	throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkHandler_t handler;
    handler.devicePath = "/dev/usb-ffs/depthai_device";
    handler.protocol = X_LINK_USB_EP;
    auto serverRet = XLinkServer(&handler, "eps", X_LINK_BOOTED, X_LINK_MYRIAD_X);
    printf("Connection returned: %s\n", XLinkErrorToStr(serverRet));

    // loop through streams
    auto s = XLinkOpenStream(0, "test_0", sizeof(DUMMY_DATA));
    if(s == INVALID_STREAM_ID){
	printf("Open stream failed...\n");
    } else {
	printf("Open stream OK - id: 0x%08X\n",  s);
    }

    auto w = XLinkWriteData(s, (uint8_t*) &s, sizeof(s));
    if (w == X_LINK_SUCCESS) {
	printf("Write successful: 0x%08X\n", w);
    } else {
	printf("Write failed...\n");
    }

    streamPacketDesc_t p;
    auto r = XLinkReadMoveData(s, &p);
    if (r == X_LINK_SUCCESS) {
	printf("Read successful: 0x%08X\n", w);
    } else {
	printf("Read failed...\n");
    }
    XLinkDeallocateMoveData(p.data, p.length);

    // Wait to make sure we caugth up to all requests
    std::this_thread::sleep_for(std::chrono::seconds(2));

    return 0;
}
