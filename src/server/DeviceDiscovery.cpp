#include <XLink.h>

#include <cstring>
#include <cstdint>
#include <thread>
#include <cassert>
#include <mutex>
#include <cstdio>
#include <functional>
#include <chrono>

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <Iphlpapi.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <netdb.h>
#include <ifaddrs.h>
#endif

namespace network {

    enum class DeviceState : std::uint32_t{
        INVALID = 0,
        BOOTLOADER = 3,
        BOOTED = 1,
        FLASH_BOOTED = 4,
    };

    void startDeviceDiscoveryService(std::string serial, DeviceState deviceState, std::function<void()> resetCb = nullptr);

} // namespace network

// Default Device Discovery port
static constexpr const auto DEFAULT_DEVICE_DISCOVERY_PORT = 11491;

namespace network {


    // TODO, document appropriatelly in a document somewhere
    enum class Command : uint32_t {
        NO_COMMAND = 0,
        DEVICE_DISCOVERY = 1,
        DEVICE_INFORMATION = 2,
        DEVICE_RESET = 3,
    };

    // Requests
    struct Request {
        Command command;
    };

    // Responses
    struct Response {
        Command command;
        Response(Command cmd) : command(cmd) {}
    };

    struct ResponseNoCommand : Response {
        ResponseNoCommand() : Response(Command::NO_COMMAND) {}
    };

    struct ResponseDeviceDiscovery : Response {
        ResponseDeviceDiscovery() : Response(Command::DEVICE_DISCOVERY) {}
        char mxid[32] = {0};
        uint32_t deviceState = 0;
    };

    struct ResponseDeviceInformation : Response {
        ResponseDeviceInformation() : Response(Command::DEVICE_INFORMATION) {}
        char mxid[32] = {0};
        int32_t linkSpeed = 0;
        int32_t linkFullDuplex = 0;
        int32_t gpioBootMode = 0;
    };

static std::thread serviceThread;
static std::mutex serviceMutex;
static bool initialized{false};

void startDeviceDiscoveryService(std::string serial, DeviceState deviceState, std::function<void()> resetCb){

    {
        std::unique_lock<std::mutex> lock(serviceMutex);
        if(initialized) {
            return;
        }
        initialized = true;
    }

    serviceThread = std::thread([deviceState, resetCb, serial](){
        int gpioBootMode = 0;

        const auto sleepMs = [](int ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };

        // Keep trying to bring up device discovery service
        while(true) {

            int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if(sockfd < 0) {
                printf("Couldn't open Datagram socket ...\n");

                // Retry
                sleepMs(1000);
                continue;
            }

            // Set reuse port option
            int flag = 1;
            if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) != 0) {
                perror("setsockopt fail");
            }

            struct sockaddr_in recv_addr;
            recv_addr.sin_family = AF_INET;
            recv_addr.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);
            recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if(bind(sockfd, (struct sockaddr*) &recv_addr, sizeof(recv_addr)) < 0) {
                perror("bind");
                close(sockfd);

                // Retry
                sleepMs(1000);
                continue;
            }

            while(true){

                // receive broadcast message from client
                Request request;
                struct sockaddr_in send_addr;
                socklen_t socklen = sizeof(send_addr);
                ssize_t packetlen = 0;
                if(( packetlen = recvfrom(sockfd, &request, sizeof(request), 0, (struct sockaddr*) &send_addr, &socklen)) < 0){
                    // Retry
                    sleepMs(100);
                    continue;
                }

                // Parse Request
                switch (request.command)
                {
                case Command::DEVICE_DISCOVERY: {

                    // send back device discovery response
                    ResponseDeviceDiscovery resp;
                    strncpy(resp.mxid, serial.c_str(), sizeof(resp.mxid));
                    resp.deviceState = static_cast<decltype(resp.deviceState)>(deviceState);

                    if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        perror("Error sending back the response");
                        // Retry
                        sleepMs(100);
                        continue;
                    }

                } break;

                case Command::DEVICE_INFORMATION: {

                    // send back device information response
                    ResponseDeviceInformation resp;
                    strncpy(resp.mxid, serial.c_str(), sizeof(resp.mxid));
                    resp.linkSpeed = 0;
                    resp.linkFullDuplex = 0;
                    resp.gpioBootMode = gpioBootMode;
                    if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        // Retry
                        sleepMs(100);
                        continue;
                    }

                } break;

                case Command::DEVICE_RESET: {
                    if(resetCb) resetCb();
                } break;

                default: {

                    // send back device information response
                    ResponseNoCommand resp;
                    if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        // Retry
                        sleepMs(100);
                        continue;
                    }

                } break;

                }

            }


        }

    });

    serviceThread.detach();

}

}

extern "C" void startDeviceDiscoveryService(const char* serial, XLinkDeviceState_t state) {
    network::DeviceState ds = network::DeviceState::INVALID;
    switch (state) {
    case X_LINK_BOOTED: ds = network::DeviceState::BOOTED; break;
    case X_LINK_BOOTLOADER: ds = network::DeviceState::BOOTLOADER; break;
    case X_LINK_FLASH_BOOTED: ds = network::DeviceState::FLASH_BOOTED; break;
    default:
            break;
    }

    if(ds != network::DeviceState::INVALID) {
        network::startDeviceDiscoveryService(std::string(serial), ds);
    }
}