/**
 * @file    tcpip_host.c
 * @brief   TCP/IP helper definitions
*/

/* **************************************************************************/
/*      Include Files                                                       */
/* **************************************************************************/
#include <string.h>
#include <time.h>

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
        return X_LINK_UNBOOTED;
    }
    else
    {
        return TCPIP_HOST_STATE_UNSUPPORTED;
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

/* **************************************************************************/
/*      Public Function Definitions                                         */
/* **************************************************************************/
tcpipHostError_t tcpip_close_socket(void* fd)
{
    intptr_t sockfd = (intptr_t)fd;
    if(sockfd != -1)
    {
        close(sockfd);
        return TCPIP_HOST_SUCCESS;
    }
    return TCPIP_HOST_ERROR;
}

tcpipHostError_t tcpip_get_ip(deviceDesc_t* devices, unsigned int* device_count, const char* target_ip)
{
    // get all network interface information
    struct ifaddrs *ifaddr;
    if(getifaddrs(&ifaddr) < 0)
    {
        return TCPIP_HOST_ERROR;
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
            socket_lookup[socket_count] = sockfd;
            socket_count++;

            // get broadcast address
            char ip_addr[MAX_IP_ADDR_CHAR] = {0};
            inet_ntop(family, &((struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr)->sin_addr, ip_addr, sizeof(ip_addr));

            // send broadcast message
            struct sockaddr_in broadcast_addr;
            broadcast_addr.sin_family = family;
            broadcast_addr.sin_addr.s_addr = inet_addr(ip_addr);
            broadcast_addr.sin_port = htons(BROADCAST_UDP_PORT);

            tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
            if(sendto(sockfd, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr)) < 0)
            {
                return TCPIP_HOST_ERROR;
            }
        }
    }

    // loop to receive message response from device
    int index = 0;
    for(int i = 0; i < socket_count; i++)
    {
        char ip_addr[MAX_IP_ADDR_CHAR] = {0};
        tcpipHostDeviceDiscoveryResp_t recv_buffer;
        struct sockaddr_in dev_addr;
        uint32_t len = sizeof(dev_addr);

        time_t start_time = time(NULL);
        do {
            if(recvfrom(socket_lookup[i], &recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr *) &dev_addr, &len) > 0)
            {
                if(recv_buffer.state != X_LINK_UNBOOTED)
                {
                    // convert IP address in binary into string
                    inet_ntop(AF_INET, &dev_addr.sin_addr, ip_addr, sizeof(ip_addr));

                    // copy device information
                    strcpy(devices[index].name, ip_addr);

                    // check if IP matched with target IP
                    if(strcmp(target_ip, ip_addr) == 0)
                    {
                        return TCPIP_HOST_DEVICE_FOUND;
                    }

                    index++;
                }
            }
        } while((time(NULL) - start_time) <= DEVICE_DISCOVERY_RES_TIMEOUT_SEC);

        // close socket
        tcpip_close_socket(&socket_lookup[i]);
    }

    // return total device found
    *device_count = index;

    return TCPIP_HOST_SUCCESS;
}
