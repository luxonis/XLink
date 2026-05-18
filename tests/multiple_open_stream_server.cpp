#include <string>
#include <vector>
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){

    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_ERROR);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    // Start server
    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    XLinkServer(&handler, "xlinkserver", X_LINK_BOOTED, X_LINK_MYRIAD_X);

    // loop through streams
    constexpr static auto NUM_STREAMS = 16;
    std::array<std::thread, NUM_STREAMS> threads;
    for(int i = 0; i < NUM_STREAMS; i++){
        threads[i] = std::thread([i](){
            std::string name = "test_";
            auto s = XLinkOpenStream(0, (name + std::to_string(i)).c_str(), 1024);
            assert(s != INVALID_STREAM_ID);
            auto w = XLinkWriteData2(s, (uint8_t*) &s, sizeof(s/2), ((uint8_t*) &s) + sizeof(s/2), sizeof(s) - sizeof(s/2));
            assert(w == X_LINK_SUCCESS);
        });
    }
    for(auto& thread : threads){
        thread.join();
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "All threads joined\n";

    return 0;
}