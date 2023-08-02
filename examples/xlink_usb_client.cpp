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

// Client
int main(int argc, char** argv) {
    if (argc != 2) {
	printf("Usage: xlink_usb_client [name of device]\n");
	exit(1);
    }

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    mvLogDefaultLevelSet(MVLOG_ERROR);

    deviceDesc_t deviceDesc;
    strcpy(deviceDesc.name, argv[1]);
    deviceDesc.protocol = X_LINK_USB_VSC;
    //    deviceDesc.protocol = X_LINK_USB_EP;

    printf("Device name: %s\n", deviceDesc.name);

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = deviceDesc.protocol;
    auto connRet = XLinkConnect(&handler);
    printf("Connection returned: %s\n", XLinkErrorToStr(connRet));

    if(connRet != X_LINK_SUCCESS) {
	return -1;
    }

    auto s = XLinkOpenStream(handler.linkId, "test_0", sizeof(DUMMY_DATA));
    if(s == INVALID_STREAM_ID){
	printf("Open stream failed...\n");
    } else {
	printf("Open stream OK - id: 0x%08X\n",  s);
    }

    streamPacketDesc_t p;
    auto r = XLinkReadMoveData(s, &p);
    if (r == X_LINK_SUCCESS) {
	printf("Read successful: 0x%08X\n", r);
    } else {
	printf("Read failed...\n");
    }
    XLinkDeallocateMoveData(p.data, p.length);

    auto w = XLinkWriteData(s, (uint8_t*) &s, sizeof(s));
    if (w == X_LINK_SUCCESS) {
	printf("Write successful: 0x%08X\n", w);
    } else {
	printf("Write failed...\n");
    }

    return 0;
}
