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


#if (defined(_WIN32) || defined(_WIN64))
#include <winsock2.h>
#include <Ws2tcpip.h>
typedef SOCKET TCPIP_SOCKET;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int TCPIP_SOCKET;
#endif


#ifdef __cplusplus
extern "C" {
#endif

/* **************************************************************************/
/*      Public Macro Definitions                                            */
/* **************************************************************************/
#define TCPIP_LINK_SOCKET_PORT              11490


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

/* **************************************************************************/
/*      Public Function Declarations                                        */
/* **************************************************************************/

/**
 * @brief Initializes TCP/IP protocol
*/
tcpipHostError_t tcpip_initialize();

/**
 * @brief       Close socket
 *
 * @param[in]   socket Socket
 * @retval      TCPIP_HOST_ERROR Failed to close socket
 * @retval      TCPIP_HOST_SUCCESS Success to close socket
*/
tcpipHostError_t tcpip_close_socket(TCPIP_SOCKET socket);

/**
 * @brief       Broadcast message and get all devices responses with their IPs
 *
 * @param[in]   state Device state which to search for
 * @param[out]  devices Pointer to store device information
 * @param[in]   devices_size Size of devices array
 * @param[out]  device_count Total device IP address obtained
 * @param[in]   target_ip Target IP address to be checked
 * @param[in]   timeout_ms Timeout in milliseconds
 * @retval      TCPIP_HOST_ERROR Failed to get network interface informations
 * @retval      TCPIP_HOST_SUCCESS Received all device IP address available
*/
xLinkPlatformErrorCode_t tcpip_get_devices(const deviceDesc_t in_deviceRequirements, deviceDesc_t* devices, size_t devices_size, unsigned int* device_count, int timeout_ms);

xLinkPlatformErrorCode_t tcpip_create_search_context(void** pctx, const deviceDesc_t in_deviceRequirements);
xLinkPlatformErrorCode_t tcpip_perform_search(void* ctx, deviceDesc_t* devices, size_t devices_size, unsigned int* device_count);
xLinkPlatformErrorCode_t tcpip_close_search_context(void* ctx);

/**
 * Send a boot to bootloader message to device with address 'name'
 * @param name device
 * @returns X_LINK_PLATFORM_SUCCESS If successfully sent boot to bootloader request or X_LINK_PLATFORM_ERROR couldn't send boot to bootloader request
*/
xLinkPlatformErrorCode_t tcpip_boot_bootloader(const char* name);

int tcpipPlatformRead(void *fd, void *data, int size);
int tcpipPlatformWrite(void *fd, void *data, int size);
int tcpipPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd);
int tcpipPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd, long *sockFd);
xLinkPlatformErrorCode_t tcpipPlatformBootBootloader(const char *name);
int tcpipPlatformDeviceFdDown(void *fd);
int tcpipPlatformClose(void *fd);
int tcpipPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length);

xLinkPlatformErrorCode_t tcpip_start_discovery_service(const char* id, XLinkDeviceState_t state, XLinkPlatform_t platform);
void tcpip_stop_discovery_service();
void tcpip_detach_discovery_service();
void tcpip_set_discovery_service_reset_callback(void (*cb)());
bool tcpip_is_running_discovery_service();

#ifdef __cplusplus
}
#endif

#endif /* TCPIP_HOST_H */
