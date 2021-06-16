/**
 * @file    tcpip_host.h
 * @brief   TCP/IP helper public header
*/

#ifndef TCPIP_HOST_H
#define TCPIP_HOST_H

/* **************************************************************************/
/*      Include Files                                                       */
/* **************************************************************************/
#include "XLinkPlatform.h"
#include "XLinkPublicDefines.h"

/* **************************************************************************/
/*      Public Macro Definitions                                            */
/* **************************************************************************/
#define TCPIP_LINK_SOCKET_PORT              11490
#define BROADCAST_UDP_PORT                  11491

#define MAX_IP_ADDR_CHAR                    64
#define MAX_MXID_CHAR                       32
#define MAX_IFACE_CHAR                      64
#define MAX_DEVICE_DISCOVERY_IFACE          10

#define MSEC_TO_USEC(x)                     (x * 1000)
#define DEVICE_DISCOVERY_RES_TIMEOUT_SEC    1   
#define DEVICE_RES_TIMEOUT_MSEC             100

/* **************************************************************************/
/*      Public Type Definitions                                             */
/* **************************************************************************/
/* TCP/IP error list */
typedef enum
{
    TCPIP_HOST_DEVICE_FOUND = 1,
    TCPIP_HOST_SUCCESS = 0,
    TCPIP_HOST_DEVICE_NOT_FOUND = -1,
    TCPIP_HOST_ERROR = -2,
    TCPIP_HOST_TIMEOUT = -3,
    TCPIP_HOST_DRIVER_NOT_LOADED = -4,
    TCPIP_INVALID_PARAMETERS = -5
} tcpipHostError_t;

/* Host-to-device command list */
typedef enum
{
    TCPIP_HOST_CMD_DEVICE_DISCOVER = 0,
    TCPIP_HOST_CMD_DEVICE_INFO = 1,
    TCPIP_HOST_CMD_RESET = 2
} tcpipHostCommand_t;

/* Device state */
typedef enum
{
    TCPIP_HOST_STATE_BOOTED = 1,
    TCPIP_HOST_STATE_BOOTLOADER = 3,
    TCPIP_HOST_STATE_UNSUPPORTED
} tcpipHostDeviceState_t;

/* Device response payload */
typedef struct
{
    char mxid[MAX_MXID_CHAR];
    uint32_t state;
} tcpipHostDeviceDiscoveryResp_t;

/* **************************************************************************/
/*      Public Function Declarations                                        */
/* **************************************************************************/

/**
 * @brief       Close socket
 * 
 * @param[in]   fd Pointer to socket
 * @retval      TCPIP_HOST_ERROR Failed to close socket
 * @retval      TCPIP_HOST_SUCCESS Success to close socket
*/
tcpipHostError_t tcpip_close_socket(void* fd);

/**
 * @brief       Broadcast message and get all devices responses with their IPs
 * 
 * @param[out]  devices Pointer to store device information
 * @param[out]  device_count Total device IP address obtained
 * @param[in]   target_ip Target IP address to be checked
 * @retval      TCPIP_HOST_ERROR Failed to get network interface informations
 * @retval      TCPIP_HOST_SUCCESS Received all device IP address available
*/
tcpipHostError_t tcpip_get_ip(deviceDesc_t* devices, unsigned int* device_count, const char* target_ip);

#endif /* TCPIP_HOST_H */