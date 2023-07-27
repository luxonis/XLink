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

int main(int argc, char** argv) {
    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);
    
    mvLogDefaultLevelSet(MVLOG_ERROR);

    deviceDesc_t deviceDesc;
    strcpy(deviceDesc.name, "1.4");
//    deviceDesc.protocol = X_LINK_USB_VSC;
    deviceDesc.protocol = X_LINK_USB_EP;

    printf("Device name: %s\n", deviceDesc.name);

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = deviceDesc.protocol;
    auto connRet = XLinkConnect(&handler);
    printf("Connection returned: %s\n", XLinkErrorToStr(connRet));
    if(connRet != X_LINK_SUCCESS) {
	return -1;
    }

    auto s = XLinkOpenStream(handler.linkId, "test_0", sizeof(DUMMY_DATA) * 2);
    if(s == INVALID_STREAM_ID){
	printf("Open stream failed...\n");
    } else {
	printf("Open stream OK - id: 0x%08X\n",  s);
    }

    auto w = XLinkWriteData(s, (uint8_t*) &s, sizeof(s));
    assert(w == X_LINK_SUCCESS);

    return 0;
}
