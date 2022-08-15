
#include <cstring>
#include <cstdint>
#include <thread>
#include <cassert>
#include <mutex>

#include <sys/types.h>
#include <sys/socketvar.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>  /* gethostbyname */

#include <net/if.h>
#include <arpa/inet.h>
//#include <sys/proc.h>
#include <sys/time.h>
#include <cstdio>
#include <functional>

#include <unistd.h>
#include <XLink.h>


namespace network {

    enum class DeviceState : std::uint32_t{
        BOOTLOADER = 3,
        BOOTED = 1,
        FLASH_BOOTED = 4,
    };

    void startDeviceDiscoveryService(DeviceState deviceState, std::function<void()> resetCb = nullptr);

} // namespace network


// Default Device Discovery port
static constexpr const auto DEFAULT_DEVICE_DISCOVERY_PORT = 11491;

extern int re_link_speed;
extern int re_link_full_d;

static const char default_serial_str[] = "deadbeef";

namespace network {

static std::thread serviceThread;
static std::mutex serviceMutex;
static bool initialized{false};
void startDeviceDiscoveryService(DeviceState deviceState, std::function<void()> resetCb){
    {
        std::unique_lock<std::mutex> lock(serviceMutex);
        if(initialized) {
            return;
        }
        initialized = true;
    }

    serviceThread = std::thread([deviceState, resetCb](){
        // TODO(themarpe) - afaik crashes in FW
        int gpioBootMode = 0x3; //getGpioBootstrap();

        const char* serial_str = "xlinkserver";
        if(serial_str == nullptr){
            serial_str = default_serial_str;
        }

        // printf("Started device discovery service!\n");

        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if(sockfd < 0)
        {
            printf("Couldn't ..\n");
            return -1;
        }

        struct sockaddr_in recv_addr;
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);
        recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if(bind(sockfd, (struct sockaddr*) &recv_addr, sizeof(recv_addr)) < 0)
        {
            perror("bind");
            close(sockfd);
            return -1;
        }

        // printf("Waiting broadcast message..\n");

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


        while(true){

            // receive broadcast message from client
            Request request;
            struct sockaddr_in send_addr;
            socklen_t socklen = sizeof(send_addr);
            ssize_t packetlen = 0;
            if(( packetlen = recvfrom(sockfd, &request, sizeof(request), 0, (struct sockaddr*) &send_addr, &socklen)) < 0){
                printf("Error recvform...\n");
                perror("recvfrom");
                close(sockfd);
                return -1;
            }

            // // Debug
            // printf("Received packet, length: %d data: ", packetlen);
            // for(ssize_t i = 0; i < packetlen; i++){
            //     printf("%02X ", ((uint8_t*)&request)[i]);
            // }
            // printf("\n");

            // Parse Request
            switch (request.command)
            {
            case Command::DEVICE_DISCOVERY: {

                printf("Received device discovery request, sending back - mxid: %s, state: %u\n", serial_str, (uint32_t) deviceState);

                // send back device discovery response
                ResponseDeviceDiscovery resp;
                strncpy(resp.mxid, serial_str, sizeof(resp.mxid));
                resp.deviceState = static_cast<decltype(resp.deviceState)>(deviceState);

                if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                    perror("sendto");
                    close(sockfd);
                    return -1;
                }

            } break;

            case Command::DEVICE_INFORMATION: {

                // printf("Received device information request, sending back - mxid: %s, speed %d, full duplex: %d, boot mode: 0x%02X\n", serial_str, re_link_speed, re_link_full_d, gpioBootMode);
                printf("Received device information request, sending back - mxid: %s, speed %d?, full duplex: %d?, boot mode: 0x%02X\n", serial_str, 0,0, gpioBootMode);

                // send back device information response
                ResponseDeviceInformation resp;
                strncpy(resp.mxid, serial_str, sizeof(resp.mxid));
                resp.linkSpeed = 0;
                // resp.linkSpeed = re_link_speed;
                // resp.linkFullDuplex = re_link_full_d;
                resp.gpioBootMode = gpioBootMode;
                if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                    perror("sendto");
                    close(sockfd);
                    return -1;
                }

            } break;

            case Command::DEVICE_RESET: {
                if(resetCb) resetCb();
            } break;

            default: {

                printf("Received invalid request, sending back no_command\n");

                // send back device information response
                ResponseNoCommand resp;
                if(sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                    perror("sendto");
                    close(sockfd);
                    return -1;
                }

            } break;

            }

        }
    });

    serviceThread.detach();

}


}


extern "C" void startDeviceDiscoveryService(XLinkDeviceState_t state) {
    network::DeviceState ds;
    switch (state) {
    case XLINK_BOOTED: ds = network::DeviceState::BOOTED; break;
    case XLINK_BOOTLOADER: ds = network::DeviceState::BOOTLOADER; break;
    case XLINK_FLASH_BOOTED: ds = network::DeviceState::FLASH_BOOTED; break;
    default:
        assert(0 && "invalid state");
        break;
    }

    network::startDeviceDiscoveryService(ds);
}

