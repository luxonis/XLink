#undef NDEBUG
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

using namespace std::chrono;

constexpr static int NUM_ITERATIONS = 10000;
constexpr static bool PRINT_DEBUG = false;
constexpr static microseconds RTT_THRESHOLD{5000};

struct Timestamp {
    int64_t sec;
    int64_t nsec;
};

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

    std::this_thread::sleep_for(milliseconds(100));
    auto s = XLinkOpenStream(handler.linkId, "rtt", 1024);

    if(s != INVALID_STREAM_ID) {
        Timestamp ts = {};

        streamPacketDesc_t* packet;
        bool success = true;

        for(int i = 1; i <= NUM_ITERATIONS; i++){
            ts.sec = i;
            ts.nsec = 0;
            auto t1 = steady_clock::now();
            assert(XLinkWriteData(s, reinterpret_cast<uint8_t*>(&ts), sizeof(ts)) == X_LINK_SUCCESS);
            auto t1point5 = steady_clock::now();
            assert(XLinkReadData(s, &packet) == X_LINK_SUCCESS);
            auto t2 = steady_clock::now();
            assert(packet->length == sizeof(ts));
            memcpy(&ts, packet->data, packet->length);
            XLinkReleaseData(s);

            if(PRINT_DEBUG) printf("client received - sec: %ld, nsec: %ld\n", ts.sec, ts.nsec);
            assert((ts.sec + 100)*2 == ts.nsec);

            if(t2-t1 <= RTT_THRESHOLD) {
                if(PRINT_DEBUG) printf("OK, rtt = %ldus. (write: %ldus)\n", duration_cast<microseconds>(t2-t1).count(), duration_cast<microseconds>(t1point5-t1).count());
            } else {
                printf("NOK, rtt = %ldus. RTT too high (write: %ldus)\n", duration_cast<microseconds>(t2-t1).count(), duration_cast<microseconds>(t1point5-t1).count());
                success = false;
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
    std::this_thread::sleep_for(milliseconds(100));

    if(s != INVALID_STREAM_ID) {
        for(int i = 1; i <= NUM_ITERATIONS; i++) {
            Timestamp timestamp = {};
            streamPacketDesc_t* packet;
            auto t1 = steady_clock::now();
            if(XLinkReadData(s, &packet) != X_LINK_SUCCESS) {
                printf("failed.\n");
                return -1;
            }
            assert(packet->length == sizeof(timestamp));
            memcpy(&timestamp, packet->data, packet->length);
            XLinkReleaseData(s);
            timestamp.nsec = (timestamp.sec + 100LL) * 2LL;
            auto t1point5 = steady_clock::now();
            if(PRINT_DEBUG) printf("server sent - sec: %ld, nsec: %ld\n", timestamp.sec, timestamp.nsec);
            if(XLinkWriteData(s, reinterpret_cast<uint8_t*>(&timestamp), sizeof(timestamp)) != X_LINK_SUCCESS) {
                printf("failed.\n");
                return -1;
            }
            auto t2 = steady_clock::now();
            if(PRINT_DEBUG) printf("Respond time: %ldus, (write: %ldus)\n", duration_cast<microseconds>(t2-t1).count(), duration_cast<microseconds>(t1point5-t1).count());
        }

    } else {
        printf("failed.\n");
        return -1;
    }

    return 0;
}
