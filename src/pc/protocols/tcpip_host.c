/**
 * @file    tcpip_host.c
 * @brief   TCP/IP helper definitions
*/

/* **************************************************************************/
/*      Include Files                                                       */
/* **************************************************************************/
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>

#include "tcpip_host.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <netdb.h>
#include <ifaddrs.h>

/* **************************************************************************/
/*      Private Macro Definitions                                            */
/* **************************************************************************/

// Debug, uncomment first line for some printing
//#define DEBUG(...) do { printf(__VA_ARGS__); } while(0)
#define DEBUG(fmt, ...) do {} while(0)

#define TCPIP_LINK_SOCKET_PORT              11490
#define BROADCAST_UDP_PORT                  11491

#define MAX_IP_ADDR_CHAR                    64
#define MAX_IFACE_CHAR                      64
#define MAX_DEVICE_DISCOVERY_IFACE          10

#define MSEC_TO_USEC(x)                     (x * 1000)
#define DEVICE_DISCOVERY_RES_TIMEOUT_SEC    0.2
#define DEVICE_RES_TIMEOUT_MSEC             20


/* **************************************************************************/
/*      Private Function Definitions                                        */
/* **************************************************************************/
static XLinkDeviceState_t tcpip_convert_device_state(uint32_t state)
{
    if(state == TCPIP_HOST_STATE_BOOTED)
    {
        return X_LINK_BOOTED;
    }
    else if(state == TCPIP_HOST_STATE_BOOTLOADER)
    {
        return X_LINK_BOOTLOADER;
    }
    else if(state == TCPIP_HOST_STATE_FLASH_BOOTED)
    {
        return X_LINK_FLASH_BOOTED;
    }
    else
    {
        return X_LINK_ANY_STATE;
    }
}

static tcpipHostError_t tcpip_create_socket(char* iface_name, int* fd)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    // add socket option for broadcast
    int rc = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &rc, sizeof(rc)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    int reuse_addr = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = MSEC_TO_USEC(DEVICE_RES_TIMEOUT_MSEC);
    if(setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    *fd = sockfd;
    return TCPIP_HOST_SUCCESS;
}

// Static
static inline double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}

/* **************************************************************************/
/*      Public Function Definitions                                         */
/* **************************************************************************/
tcpipHostError_t tcpip_close_socket(int sockfd)
{
    if(sockfd != -1)
    {
        close(sockfd);
        return TCPIP_HOST_SUCCESS;
    }
    return TCPIP_HOST_ERROR;
}

xLinkPlatformErrorCode_t tcpip_get_devices(XLinkDeviceState_t state, deviceDesc_t* devices, ssize_t devices_size, unsigned int* device_count, const char* target_ip)
{

    bool check_target_ip = false;
    if(target_ip != NULL && strlen(target_ip) > 0){
        check_target_ip = true;
    }

    // get all network interface information
    struct ifaddrs *ifaddr;
    if(getifaddrs(&ifaddr) < 0)
    {
        return X_LINK_PLATFORM_ERROR;
    }

    // iterate linked list of interface information
    int family;
    int socket_count = 0;
    int socket_lookup[MAX_DEVICE_DISCOVERY_IFACE] = {0};
    for(struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        // check for ipv4 family
        family = ifa->ifa_addr->sa_family;
        if(family == AF_INET)
        {

            // create new socket for every interface
            int sockfd = -1;
            tcpip_create_socket(ifa->ifa_name, &sockfd);

            // Check if interface is up and running
            struct ifreq if_req;
            strncpy(if_req.ifr_name, ifa->ifa_name, sizeof(if_req.ifr_name));
            ioctl(sockfd, SIOCGIFFLAGS, &if_req);

            DEBUG("interface name %s, (flags: %hu). ", ifa->ifa_name, if_req.ifr_flags);

            if((if_req.ifr_flags & IFF_UP) && (if_req.ifr_flags & IFF_RUNNING)){
                // Interface is up and running, save the socket
                socket_lookup[socket_count] = sockfd;
                socket_count++;

                // get broadcast address
                char ip_addr[MAX_IP_ADDR_CHAR] = {0};
                inet_ntop(family, &((struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr)->sin_addr, ip_addr, sizeof(ip_addr));

                DEBUG("Up and running. IP: %s", ip_addr);

                // send broadcast message
                struct sockaddr_in broadcast_addr;
                broadcast_addr.sin_family = family;
                broadcast_addr.sin_addr.s_addr = inet_addr(ip_addr);
                broadcast_addr.sin_port = htons(BROADCAST_UDP_PORT);

                tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
                if(sendto(sockfd, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr)) < 0)
                {
                    // Ignore if not successful. The devices on that interface won't be found
                }
            } else {
                DEBUG("Not up and running.");

                // Close socket
                tcpip_close_socket(sockfd);
            }

            DEBUG("\n");

        }
    }

    // loop to receive message response from devices
    int num_devices_match = 0;

    // Loop through all sockets and received messages that arrived
    double t1 = seconds();
    do {
        for(int i = 0; i < socket_count; i++)
        {

            if(num_devices_match >= devices_size){
                // Enough devices matched, exit the loop
                break;
            }

            char ip_addr[MAX_IP_ADDR_CHAR] = {0};
            tcpipHostDeviceDiscoveryResp_t recv_buffer;
            struct sockaddr_in dev_addr;
            uint32_t len = sizeof(dev_addr);

            if(recvfrom(socket_lookup[i], &recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr *) &dev_addr, &len) > 0)
            {
                if(recv_buffer.command == TCPIP_HOST_CMD_DEVICE_DISCOVER && state == tcpip_convert_device_state(recv_buffer.state))
                {
                    // Correct device found, increase matched num and save details

                    // convert IP address in binary into string
                    inet_ntop(AF_INET, &dev_addr.sin_addr, ip_addr, sizeof(ip_addr));

                    // Check IP if needed
                    if(check_target_ip && strcmp(target_ip, ip_addr) != 0){
                        continue;
                    }

                    // copy device information
                    strncpy(devices[num_devices_match].name, ip_addr, sizeof(devices[num_devices_match].name));

                    num_devices_match++;
                }
            }
        }
        if(num_devices_match >= devices_size){
            // Enough devices matched, exit the loop
            break;
        }
    } while(seconds() - t1 < DEVICE_DISCOVERY_RES_TIMEOUT_SEC);

    // Close the sockets
    for(int i = 0; i < socket_count; i++)
    {
        // close socket
        tcpip_close_socket(socket_lookup[i]);
    }

    // return total device found
    *device_count = num_devices_match;

    // if at least one device matched, return OK otherwise return not found
    if(num_devices_match <= 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    return X_LINK_PLATFORM_SUCCESS;
}
