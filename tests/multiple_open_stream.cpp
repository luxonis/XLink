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

// The following is an Server side (host) test that opens many streams in random order.
// Mainly used to test stream desync issue.

// Use the following code on Client side (device) to test
// ...
//    // loop through streams
//    constexpr static auto NUM_STREAMS = 16;
//    std::array<std::thread, NUM_STREAMS> threads;
//    for(int i = 0; i < NUM_STREAMS; i++){
//        threads[i] = std::thread([i](){
//            std::string name = "test_";
//            auto s = XLinkOpenStream(0, (name + std::to_string(i)).c_str(), 1024);
//            assert(s != INVALID_STREAM_ID);
//            auto w = XLinkWriteData(s, (uint8_t*) &s, sizeof(s));
//            assert(w == X_LINK_SUCCESS);
//        });
//    }
//    for(auto& thread : threads){
//        thread.join();
//    }
// ...

int main() {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    // Search for booted device
    deviceDesc_t deviceDesc, inDeviceDesc;
    inDeviceDesc.protocol = X_LINK_ANY_PROTOCOL;
    inDeviceDesc.state = X_LINK_BOOTED;
    if(X_LINK_SUCCESS != XLinkFindFirstSuitableDevice(inDeviceDesc, &deviceDesc)){
        printf("Didn't find a device\n");
        return -1;
    }

    printf("Device name: %s\n", deviceDesc.name);

    XLinkHandler_t handler;
    handler.devicePath = deviceDesc.name;
    handler.protocol = deviceDesc.protocol;
    XLinkConnect(&handler);

    // loop randomly over streams
    constexpr static auto NUM_STREAMS = 16;
    std::vector<int> randomized;
    for(int i = 0; i < NUM_STREAMS; i++){
        randomized.push_back(i);
    }
    std::random_shuffle(std::begin(randomized), std::end(randomized));

    std::thread threads[NUM_STREAMS];
    streamId_t streams[NUM_STREAMS];
    for(auto i : randomized){
        threads[i] = std::thread([&, i](){
            std::string name = "test_" + std::to_string(i);
            auto s = XLinkOpenStream(handler.linkId, name.c_str(), 1024);
            if(s == INVALID_STREAM_ID){
                printf("Open stream failed...\n");
            } else {
                printf("Open stream OK - name %s, id: 0x%08X\n", name.c_str(), s);
            }
            streams[i] = s;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(auto i : randomized){
        threads[i].join();
    }

    // Optionally, print stream names and ids here
    std::atomic<bool> success{true};
    for(auto i : randomized){
        threads[i] = std::thread([i, &streams, &success](){
            std::string name = "test_" + std::to_string(i);
            auto s = streams[i];

            streamPacketDesc_t* p;
            XLinkError_t err = XLinkReadData(s, &p);

            if(err == X_LINK_SUCCESS && p && p->data && s == *((streamId_t*) p->data)) {
                // OK
            } else {
                streamId_t id;
                memcpy(&id, p->data, sizeof(id));
                printf("DESYNC error - name %s, id: 0x%08X, response id: 0x%08X\n", name.c_str(), s, id);
                success = false;
            }

        });
    }
    for(auto i : randomized){
        threads[i].join();
    }

    XLinkResetRemote(handler.linkId);

    if(success){
        printf("Success!\n");
        return 0;
    } else {
        printf("Error!\n");
        return -1;
    }
}