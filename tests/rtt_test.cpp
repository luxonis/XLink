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
#include <condition_variable>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"


struct Timestamp {
    int64_t sec;
    int64_t nsec;
};
using namespace std::chrono;

int client();
int server();

bool successServer{true};
bool successClient{true};

int main(int argc, char** argv) {
    // mvLogDefaultLevelSet(MVLOG_DEBUG);

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);
    std::thread ts([](){successServer = server() == 0;});
    std::this_thread::sleep_for(milliseconds(100));
    std::thread tc([](){successClient = client() == 0;});

    ts.join();
    tc.join();

    if(successServer && successClient) {
        return 0;
    } else {
        return -1;
    }
}

// Client
int client() {
    deviceDesc_t deviceDesc;
    strcpy(deviceDesc.name, "127.0.0.1");
    deviceDesc.protocol = X_LINK_TCP_IP;

    printf("Device name: %s\n", deviceDesc.name);

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = deviceDesc.protocol;
    XLinkConnect(&handler);

    auto s = XLinkOpenStream(handler.linkId, "rtt", 1024);
    if(s != INVALID_STREAM_ID) {
        Timestamp ts = {};

        streamPacketDesc_t* packet;
        bool success = true;
        for(int i = 0; i < 100; i++){
            auto t1 = steady_clock::now();
            XLinkWriteData(s, reinterpret_cast<uint8_t*>(&ts), sizeof(ts));
            auto t1point5 = steady_clock::now();
            XLinkReadData(s, &packet);
            auto t2 = steady_clock::now();
            XLinkReleaseData(s);

            if(t2-t1 <= milliseconds(1)) {
                printf("OK, rtt = %ldms. RTT too high (write: %ld)\n", duration_cast<milliseconds>(t2-t1).count(), duration_cast<milliseconds>(t1point5-t1).count());
            } else {
                success = false;
                printf("NOK, rtt = %ldms. RTT too high (write: %ld)\n", duration_cast<milliseconds>(t2-t1).count(), duration_cast<milliseconds>(t1point5-t1).count());
            }
        }

        if(success) {
            printf("Success!\n");
        } else {
            printf("Failed\n");
            return -1;
        }

    } else {
        printf("Failed\n");
        return -1;
    }

    return 0;

}

// Server
XLinkGlobalHandler_t xlinkGlobalHandler = {};
int server(){

    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};

    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    XLinkServer(&handler, "test", X_LINK_BOOTED, X_LINK_MYRIAD_X);
    auto s = XLinkOpenStream(handler.linkId, "rtt", 1024);
    if(s != INVALID_STREAM_ID) {

        Timestamp timestamp = {};
        for(int i = 0; i<100; i++) {
            streamPacketDesc_t* packet;
            auto t1 = steady_clock::now();
            if(XLinkReadData(s, &packet) != X_LINK_SUCCESS) {
                printf("failed.\n");
                return -1;
            }
            auto t1point5 = steady_clock::now();
            if(XLinkWriteData(s, packet->data, packet->length) != X_LINK_SUCCESS) {
                printf("failed.\n");
                return -1;
            }
            auto t2 = steady_clock::now();

            printf("Respond time: %ld, (write: %ld)\n", duration_cast<milliseconds>(t2-t1).count(), duration_cast<milliseconds>(t1point5-t1).count());

            XLinkReleaseData(s);
        }

    } else {
        printf("failed.\n");
        return -1;
    }

    return 0;
}
