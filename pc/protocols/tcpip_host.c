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
typedef int SOCKET;
#endif

/* **************************************************************************/
/*      Private Macro Definitions                                            */
/* **************************************************************************/

// Debug, uncomment first line for some printing
//#define HAS_DEBUG
#undef HAS_DEBUG

#define BROADCAST_UDP_PORT                  11491

#define MAX_IFACE_CHAR                      64
#define MAX_DEVICE_DISCOVERY_IFACE          10

#define MSEC_TO_USEC(x)                     (x * 1000)
#define DEVICE_DISCOVERY_RES_TIMEOUT_SEC    0.2
#define DEVICE_RES_TIMEOUT_MSEC             20

#ifdef HAS_DEBUG
#define DEBUG(...) do { printf(__VA_ARGS__); } while(0)
#else
#define DEBUG(fmt, ...) do {} while(0)
#endif


/* **************************************************************************/
/*      Private Function Definitions                                        */
/* **************************************************************************/
#if (defined(_WIN32) || defined(_WIN64) )
#include <win_time.h>
#endif
static inline double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}

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
    /* TODO(themarpe) - when merged with flash_booted
    else if(state == TCPIP_HOST_STATE_FLASH_BOOTED)
    {
        return X_LINK_FLASH_BOOTED;
    }
    */
    else
    {
        return X_LINK_ANY_STATE;
    }
}


static tcpipHostError_t tcpip_create_socket_broadcast(SOCKET* out_sock)
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 #if (defined(_WIN32) || defined(_WIN64) )
    if(sock == INVALID_SOCKET)
    {
        return TCPIP_HOST_ERROR;
    }
 #else
    if(sock < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // add socket option for broadcast
    int rc = 1;
    if(setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &rc, sizeof(rc)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

#if (defined(_WIN32) || defined(_WIN64) )
#else
    int reuse_addr = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // Specify timeout
#if (defined(_WIN32) || defined(_WIN64) )
    int read_timeout = DEVICE_RES_TIMEOUT_MSEC;
#else
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = MSEC_TO_USEC(DEVICE_RES_TIMEOUT_MSEC);
#endif
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    *out_sock = sock;
    return TCPIP_HOST_SUCCESS;
}



static tcpipHostError_t tcpip_send_broadcast(SOCKET sock){

#if (defined(_WIN32) || defined(_WIN64) )

    DWORD rv, size;
    PMIB_IPADDRTABLE ipaddrtable;

    rv = GetIpAddrTable(NULL, &size, 0);
    if (rv != ERROR_INSUFFICIENT_BUFFER) {
        return TCPIP_HOST_ERROR;
    }
    ipaddrtable = (PMIB_IPADDRTABLE) malloc(size);

    rv = GetIpAddrTable(ipaddrtable, &size, 0);
    if (rv != NO_ERROR) {
        free(ipaddrtable);
        return TCPIP_HOST_ERROR;
    }

    tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
    struct sockaddr_in broadcast = { 0 };
    broadcast.sin_addr.s_addr = INADDR_BROADCAST;
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(BROADCAST_UDP_PORT);
    for (DWORD i = 0; i < ipaddrtable->dwNumEntries; ++i) {

        // Get broadcast IP
        MIB_IPADDRROW addr = ipaddrtable->table[i];
        broadcast.sin_addr.s_addr = (addr.dwAddr & addr.dwMask)
            | (addr.dwMask ^ (DWORD)0xffffffff);
        sendto(sock, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr*) & broadcast, sizeof(broadcast));

#ifdef HAS_DEBUG
        char ip_broadcast_str[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &((struct sockaddr_in*) & broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
        DEBUG("Interface up and running. Broadcast IP: %s\n", ip_broadcast_str);
#endif

    }

    free(ipaddrtable);

    return TCPIP_HOST_SUCCESS;

#else

    struct ifaddrs *ifaddr = NULL;
    if(getifaddrs(&ifaddr) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    // iterate linked list of interface information
    int family;
    int socket_count = 0;
    for(struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        // check for ipv4 family
        family = ifa->ifa_addr->sa_family;
        if(ifa->ifa_addr != NULL && family == AF_INET)
        {
            // Check if interface is up and running
            struct ifreq if_req;
            strncpy(if_req.ifr_name, ifa->ifa_name, sizeof(if_req.ifr_name));
            ioctl(sock, SIOCGIFFLAGS, &if_req);

            DEBUG("interface name %s, (flags: %hu). ", ifa->ifa_name, if_req.ifr_flags);

            if((if_req.ifr_flags & IFF_UP) && (if_req.ifr_flags & IFF_RUNNING)){
                // Interface is up and running
                // Calculate broadcast address (IPv4, OR negated mask)
                struct sockaddr_in ip_broadcast = *((struct sockaddr_in*) ifa->ifa_addr);
                struct sockaddr_in ip_netmask = *((struct sockaddr_in*) ifa->ifa_netmask);
                ip_broadcast.sin_addr.s_addr |= ~ip_netmask.sin_addr.s_addr;

                #ifdef HAS_DEBUG
                    char ip_broadcast_str[INET_ADDRSTRLEN] = {0};
                    inet_ntop(family, &((struct sockaddr_in *)&ip_broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
                    DEBUG("Up and running. Broadcast IP: %s", ip_broadcast_str);
                #endif

                // send broadcast message
                struct sockaddr_in broadcast_addr;
                broadcast_addr.sin_family = family;
                broadcast_addr.sin_addr.s_addr = ip_broadcast.sin_addr.s_addr;
                broadcast_addr.sin_port = htons(BROADCAST_UDP_PORT);

                tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
                if(sendto(sock, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr)) < 0)
                {
                    // Ignore if not successful. The devices on that interface won't be found
                }
            } else {
                DEBUG("Not up and running.");
            }

            DEBUG("\n");

        }
    }
    // Release interface addresses
    freeifaddrs(ifaddr);

    return TCPIP_HOST_SUCCESS;
#endif
}


/* **************************************************************************/
/*      Public Function Definitions                                         */
/* **************************************************************************/
tcpipHostError_t tcpip_close_socket(SOCKET sock)
{
#if (defined(_WIN32) || defined(_WIN64) )
    if(sock != INVALID_SOCKET)
    {
        closesocket(sock);
        return TCPIP_HOST_SUCCESS;
    }
#else
    if(sock != -1)
    {
        close(sock);
        return TCPIP_HOST_SUCCESS;
    }
#endif
    return TCPIP_HOST_ERROR;
}

xLinkPlatformErrorCode_t tcpip_get_devices(XLinkDeviceState_t state, deviceDesc_t* devices, size_t devices_size, unsigned int* device_count, const char* target_ip)
{

    bool check_target_ip = false;
    if(target_ip != NULL && strlen(target_ip) > 0){
        check_target_ip = true;
    }

    // Create ANY receiving socket first
    SOCKET sock;
    if(tcpip_create_socket_broadcast(&sock) != TCPIP_HOST_SUCCESS){
        return X_LINK_PLATFORM_ERROR;
    }

    // Then send broadcast
    if (tcpip_send_broadcast(sock) != TCPIP_HOST_SUCCESS) {
        return X_LINK_PLATFORM_ERROR;
    }

    // loop to receive message response from devices
    int num_devices_match = 0;
    // Loop through all sockets and received messages that arrived
    double t1 = seconds();
    do {
        if(num_devices_match >= devices_size){
            // Enough devices matched, exit the loop
            break;
        }

        char ip_addr[INET_ADDRSTRLEN] = {0};
        tcpipHostDeviceDiscoveryResp_t recv_buffer = {0};
        struct sockaddr_in dev_addr;
        uint32_t len = sizeof(dev_addr);

        int ret = recvfrom(sock, &recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr*) & dev_addr, &len);
        if(ret > 0)
        {
            DEBUG("Received UDP response, length: %d\n", ret);
            if(recv_buffer.command == TCPIP_HOST_CMD_DEVICE_DISCOVER && state == tcpip_convert_device_state(recv_buffer.state))
            {
                // Correct device found, increase matched num and save details

                // convert IP address in binary into string
                inet_ntop(AF_INET, &dev_addr.sin_addr, ip_addr, sizeof(ip_addr));
                // if(state == X_LINK_BOOTED){
                //     strncat(ip_addr, ":11492", 16);
                // }

                // Check IP if needed
                if(check_target_ip && strcmp(target_ip, ip_addr) != 0){
                    continue;
                }

                // copy device information
                strncpy(devices[num_devices_match].name, ip_addr, sizeof(devices[num_devices_match].name));
                devices[num_devices_match].platform = X_LINK_MYRIAD_X;
                devices[num_devices_match].protocol = X_LINK_TCP_IP;

                num_devices_match++;
            }
        }
    } while(seconds() - t1 < DEVICE_DISCOVERY_RES_TIMEOUT_SEC);

    tcpip_close_socket(sock);

    // return total device found
    *device_count = num_devices_match;

    // if at least one device matched, return OK otherwise return not found
    if(num_devices_match <= 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    return X_LINK_PLATFORM_SUCCESS;
}
