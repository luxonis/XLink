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


#ifdef XLINK_TEST_CLIENT
// Client
int main(int argc, char** argv) {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    int numConnections = argc - 1;
    std::vector<std::thread> connections;
    std::atomic<bool> allSuccess{true};
    for(int connection = 0; connection < numConnections; connection++) {
        connections.push_back(std::thread([connection, &allSuccess, argv](){

            deviceDesc_t deviceDesc;
            strcpy(deviceDesc.name, argv[connection+1]);
            deviceDesc.protocol = X_LINK_TCP_IP;

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
                        printf("Open stream OK - conn: %d, name: %s, id: 0x%08X\n", connection, name.c_str(), s);
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
                threads[i] = std::thread([connection, i, &streams, &success](){
                    std::string name = "test_" + std::to_string(i);
                    auto s = streams[i];

                    streamPacketDesc_t* p;
                    XLinkError_t err = XLinkReadData(s, &p);

                    if(err == X_LINK_SUCCESS && p && p->data && ((s & 0xFFFFFF) == *((streamId_t*) p->data))) {
                        // OK
                    } else {
                        streamId_t id = 0;
                        if(p != nullptr) {
                            memcpy(&id, p->data, sizeof(id));
                        }

                        printf("DESYNC error - err: %s, conn: %d, name: %s, id: 0x%08X, response id: 0x%08X\n", XLinkErrorToStr(err), connection, name.c_str(), s, id);
                        success = false;
                    }

                });
            }
            for(auto i : randomized){
                threads[i].join();
            }

            success = XLinkResetRemote(handler.linkId) == X_LINK_SUCCESS;

            if(!success){
                allSuccess = false;
            }

        }));

        // std::this_thread::sleep_for(std::chrono::seconds(1));

    }

    for(auto& conn : connections){
        conn.join();
    }

}

#endif


#ifdef XLINK_TEST_SERVER
// Server
XLinkGlobalHandler_t xlinkGlobalHandler = {};
int main(int argc, const char** argv){

    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_ERROR);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};
    if(argc > 1) {
        serverIp = std::string(argv[1]);
    }
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    XLinkServer(&handler, X_LINK_BOOTED, X_LINK_MYRIAD_X);

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
#endif