#include <XLink/XLink.h>
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <stdexcept>
#include <iostream>
#include <array>
#include <mutex>
#include <condition_variable>

std::mutex coutMtx;

std::mutex startMtx;
std::condition_variable startCv;
bool startBool;

std::atomic<int> errorRet{0};

int main() {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    // Search for booted device
    deviceDesc_t deviceDesc, inDeviceDesc;
    inDeviceDesc.protocol = X_LINK_ANY_PROTOCOL;

    // Get all available devices normally
    unsigned int numdevNonThreaded = 0;
    std::array<deviceDesc_t, 32> deviceDescAllNonThreaded = {};
    deviceDesc_t suitableDevice = {};
    suitableDevice.protocol = X_LINK_ANY_PROTOCOL;
    suitableDevice.platform = X_LINK_ANY_PLATFORM;
    auto status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAllNonThreaded.data(), deviceDescAllNonThreaded.size(), &numdevNonThreaded);
    if(status != X_LINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");
    std::sort(deviceDescAllNonThreaded.begin(), deviceDescAllNonThreaded.begin() + numdevNonThreaded, [](const deviceDesc_t& a, const deviceDesc_t& b){
        return std::string(b.name) > std::string(a.name);
    });

    // Print device details
    for(int i = 0; i < numdevNonThreaded; i++){
        const auto& dev = deviceDescAllNonThreaded[i];
        std::cout << "status: " << XLinkErrorToStr(dev.status);
        std::cout << ", name: " << dev.name;
        std::cout << ", mxid: " << dev.mxid;
        std::cout << ", state: " << XLinkDeviceStateToStr(dev.state);
        std::cout << ", protocol: " << XLinkProtocolToStr(dev.protocol);
        std::cout << ", platform: " << XLinkPlatformToStr(dev.platform);
        std::cout << std::endl;
    }
    std::cout << std::endl;


    // loop randomly over streams
    constexpr static auto NUM_STREAMS = 128;
    std::vector<int> randomized;
    std::vector<int> results;
    results.resize(NUM_STREAMS);
    for(int i = 0; i < NUM_STREAMS; i++){
        randomized.push_back(i);
    }
    std::random_shuffle(std::begin(randomized), std::end(randomized));

    std::thread threads[NUM_STREAMS];
    streamId_t streams[NUM_STREAMS];
    for(auto i : randomized){
        threads[i] = std::thread([&, i](){
            // Get all available devices
            unsigned int numdev = 0;
            std::array<deviceDesc_t, 32> deviceDescAll = {};
            deviceDesc_t suitableDevice = {};
            suitableDevice.protocol = X_LINK_ANY_PROTOCOL;
            suitableDevice.platform = X_LINK_ANY_PLATFORM;

            {
                std::unique_lock<std::mutex> l(startMtx);
                startCv.wait(l, [](){
                    return startBool;
                });
            }

            auto status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
            if(status != X_LINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

            // Print device details
            {
                std::unique_lock<std::mutex> l(coutMtx);
                std::cout << "thread: " << i << " numdev: " << numdev << std::endl;
                for(int i = 0; i < numdev; i++){
                    const auto& dev = deviceDescAll[i];
                    std::cout << "status: " << XLinkErrorToStr(dev.status);
                    std::cout << ", name: " << dev.name;
                    std::cout << ", mxid: " << dev.mxid;
                    std::cout << ", state: " << XLinkDeviceStateToStr(dev.state);
                    std::cout << ", protocol: " << XLinkProtocolToStr(dev.protocol);
                    std::cout << ", platform: " << XLinkPlatformToStr(dev.platform);
                    std::cout << std::endl;
                }
                std::cout << std::endl;
            }

            // Make sure all threads found same devices
            if(numdev == numdevNonThreaded){
                // Sort first
                std::sort(deviceDescAll.begin(), deviceDescAll.begin() + numdev, [](const deviceDesc_t& a, const deviceDesc_t& b){
                    return std::string(b.name) > std::string(a.name);
                });

                // compare directly
                for(int i = 0; i < numdev; i++) {
                    if(std::string(deviceDescAll[i].name) != std::string(deviceDescAllNonThreaded[i].name)) {
                        printf("Rip, dev[%d] name: %s, non threaded: %s\n",i,  deviceDescAll[i].name, deviceDescAllNonThreaded[i].name);
                        errorRet = 2;
                    }
                    if(std::string(deviceDescAll[i].mxid) != std::string(deviceDescAllNonThreaded[i].mxid)) {
                        printf("Rip, dev[%d] mxid: %s, non threaded: %s\n", i, deviceDescAll[i].mxid, deviceDescAllNonThreaded[i].mxid);
                        errorRet = 3;
                    }
                    if(deviceDescAll[i].state != deviceDescAllNonThreaded[i].state) {
                        printf("Rip, dev[%d] state: %d, non threaded: %d\n", i, deviceDescAll[i].state, deviceDescAllNonThreaded[i].state);
                        errorRet = 4;
                    }
                    if(deviceDescAll[i].protocol != deviceDescAllNonThreaded[i].protocol) {
                        printf("Rip, dev[%d] protocol: %d, non threaded: %d\n", i, deviceDescAll[i].protocol, deviceDescAllNonThreaded[i].protocol);
                        errorRet = 5;
                    }
                    if(deviceDescAll[i].platform != deviceDescAllNonThreaded[i].platform) {
                        printf("Rip, dev[%d] platform: %d, non threaded: %d\n", i, deviceDescAll[i].platform, deviceDescAllNonThreaded[i].platform);
                        errorRet = 6;
                    }
                }
            } else {
                // printf("RIP\n\n");
                errorRet = 1;
            }
        });
    }

    // Fire off all threads at the same time
    // prepare time for threads
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::unique_lock<std::mutex> l(startMtx);
        startBool = true;
    }
    startCv.notify_all();


    for(auto i : randomized){
        threads[i].join();
    }

    if(errorRet != 0) {
        std::cout << "Failed, not all threads found same devices\n";
        return errorRet;
    }

    return 0;
}