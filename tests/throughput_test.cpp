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
constexpr static size_t THROUGHPUT_THRESHOLD{1000*1024*1024};

constexpr static size_t BUFFER_SIZE = 1024*1024*2;

struct Timestamp {
    int64_t sec;
    int64_t nsec;
};

int client(bool split);
int server(bool split);


int main(int argc, char** argv) {
    // mvLogDefaultLevelSet(MVLOG_DEBUG);

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);
    bool successServer{true};
    bool successClient{true};

    {
        std::thread ts([&successServer](){successServer = server(false) == 0;});
        std::this_thread::sleep_for(milliseconds(100));
        std::thread tc([&successClient](){successClient = client(false) == 0;});

        ts.join();
        tc.join();

        if(!successServer || !successClient) {
            return -1;
        }
    }

    {
        std::thread ts([&successServer](){successServer = server(true) == 0;});
        std::this_thread::sleep_for(milliseconds(100));
        std::thread tc([&successClient](){successClient = client(true) == 0;});

        ts.join();
        tc.join();

        if(!successServer || !successClient) {
            return -1;
        }
    }

    return 0;
}

// Client
int client(bool split) {
    deviceDesc_t deviceDesc;
    strcpy(deviceDesc.name, "127.0.0.1");
    deviceDesc.protocol = X_LINK_TCP_IP;

    printf("Device name: %s\n", deviceDesc.name);

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = deviceDesc.protocol;
    assert(XLinkConnect(&handler) == X_LINK_SUCCESS);

    std::this_thread::sleep_for(milliseconds(100));
    auto s = XLinkOpenStream(handler.linkId, "rtt", 2*BUFFER_SIZE);
    assert(s != INVALID_STREAM_ID);

    Timestamp ts = {};
    uint8_t buffer[BUFFER_SIZE];

    auto t1 = steady_clock::now();
    for(int i = 1; i <= NUM_ITERATIONS; i++){
        if(split) {
            assert(XLinkWriteData2(s, buffer, BUFFER_SIZE, reinterpret_cast<uint8_t*>(&ts), sizeof(ts)) == X_LINK_SUCCESS);
        } else {
            assert(XLinkWriteData(s, buffer, BUFFER_SIZE) == X_LINK_SUCCESS);
        }
    }

    streamPacketDesc_t* packet;
    assert(XLinkReadData(s, &packet) == X_LINK_SUCCESS);
    auto t2 = steady_clock::now();
    assert(XLinkReleaseData(s) == X_LINK_SUCCESS);

    size_t throughput = (BUFFER_SIZE*NUM_ITERATIONS) / duration_cast<duration<double>>(t2-t1).count();

    if(throughput > THROUGHPUT_THRESHOLD) {
        printf("Success - throughput: %ldMiB/s!\n", throughput/(1024*1024));
    } else {
        printf("Fail - throughput: %ldMiB/s!\n", throughput/(1024*1024));
        return -1;
    }

    assert(XLinkCloseStream(s) == X_LINK_SUCCESS);
    assert(XLinkResetRemote(handler.linkId) == X_LINK_SUCCESS);

    return 0;

}

// Server
XLinkGlobalHandler_t xlinkGlobalHandler = {};
int server(bool split){
    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};

    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    XLinkServerOnly(&handler);
    auto s = XLinkOpenStream(handler.linkId, "rtt", 2*BUFFER_SIZE);
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
            XLinkReleaseData(s);
        }
        uint8_t tmp[4];
        assert(XLinkWriteData(s, reinterpret_cast<uint8_t*>(&tmp), sizeof(tmp)) == X_LINK_SUCCESS);

    } else {
        printf("failed.\n");
        return -1;
    }

    assert(XLinkCloseStream(s) == X_LINK_SUCCESS);
    assert(XLinkResetRemote(handler.linkId) == X_LINK_SUCCESS);


    return 0;
}
