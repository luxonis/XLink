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

int main() {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    // Search for booted device
    deviceDesc_t deviceDesc, inDeviceDesc;
    inDeviceDesc.protocol = X_LINK_ANY_PROTOCOL;

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

            auto status = XLinkFindAllSuitableDevices(suitableDevice, deviceDescAll.data(), deviceDescAll.size(), &numdev);
            if(status != X_LINK_SUCCESS) throw std::runtime_error("Couldn't retrieve all connected devices");

            // Print device details
            std::cout << "numdev: " + std::to_string(numdev) + "\n";
            results[i] = numdev;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(auto i : randomized){
        threads[i].join();
    }

    int numdev = -1;
    for(auto num : results) {
        if(numdev == -1) numdev = num;
        // make sure all threads found the same number of devices
        if(num != numdev) {
            std::cout << "Failed, not all threads found same number of devices\n";
            return -1;
        }
    }

}