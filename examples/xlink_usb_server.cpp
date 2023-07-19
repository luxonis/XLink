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
constexpr static auto NUM_STREAMS = 16;
constexpr static auto NUM_PACKETS = 120;
const uint8_t DUMMY_DATA[1024*128] = {};
XLinkGlobalHandler_t xlinkGlobalHandler = {};

// Server
//

int main(int argc, const char** argv){

    xlinkGlobalHandler.protocol = X_LINK_USB_EP;

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_ERROR);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
	throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkHandler_t handler;
    handler.devicePath = "/dev/usb-ffs/xlink"; 
    handler.protocol = X_LINK_USB_EP;
    XLinkServer(&handler, "eps", X_LINK_BOOTED, X_LINK_MYRIAD_X);


    // loop through streams
    auto s = XLinkOpenStream(0, "test_0", sizeof(DUMMY_DATA) * 2);
    assert(s != INVALID_STREAM_ID);

    //    auto w = XLinkWriteData2(s, (uint8_t*) &s, sizeof(s/2), ((uint8_t*) &s) + sizeof(s/2), sizeof(s) - sizeof(s/2));
    //    assert(w == X_LINK_SUCCESS);

    auto w = XLinkWriteData(s, (uint8_t*) &s, sizeof(s));
    assert(w == X_LINK_SUCCESS);
    
    
    streamPacketDesc_t p;
    w = XLinkReadMoveData(s, &p);
    assert(w == X_LINK_SUCCESS);
    XLinkDeallocateMoveData(p.data, p.length);
    
    return 0;
}
