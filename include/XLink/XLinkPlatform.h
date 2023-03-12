// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///
/// @brief     Application configuration Leon header
///

#ifndef _XLINK_LINKPLATFORM_H
#define _XLINK_LINKPLATFORM_H

#include "XLinkPrivateDefines.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif
#ifdef XLINK_MAX_STREAM_RES
#define MAX_POOLS_ALLOC XLINK_MAX_STREAM_RES
#else
#define MAX_POOLS_ALLOC 32
#endif
#define PACKET_LENGTH (64*1024)

typedef enum {
    X_LINK_PLATFORM_SUCCESS = 0,
    X_LINK_PLATFORM_DEVICE_NOT_FOUND = -1,
    X_LINK_PLATFORM_ERROR = -2,
    X_LINK_PLATFORM_TIMEOUT = -3,
    X_LINK_PLATFORM_INVALID_PARAMETERS = -4,
    X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS = -5,
    X_LINK_PLATFORM_DEVICE_BUSY = -6,
    X_LINK_PLATFORM_DRIVER_NOT_LOADED = -128,
    X_LINK_PLATFORM_USB_DRIVER_NOT_LOADED = X_LINK_PLATFORM_DRIVER_NOT_LOADED+X_LINK_USB_VSC,
    X_LINK_PLATFORM_TCP_IP_DRIVER_NOT_LOADED = X_LINK_PLATFORM_DRIVER_NOT_LOADED+X_LINK_TCP_IP,
    X_LINK_PLATFORM_PCIE_DRIVER_NOT_LOADED = X_LINK_PLATFORM_DRIVER_NOT_LOADED+X_LINK_PCIE,
} xLinkPlatformErrorCode_t;

// ------------------------------------
// Device management. Begin.
// ------------------------------------

xLinkPlatformErrorCode_t XLinkPlatformInit(void* options);

#ifndef __DEVICE__
/**
 * @brief Return Myriad device description which meets the requirements
 */
xLinkPlatformErrorCode_t XLinkPlatformFindDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, unsigned sizeFoundDevices,
                                                     unsigned *out_amountOfFoundDevices);

xLinkPlatformErrorCode_t XLinkPlatformFindArrayOfDevicesNames(
    XLinkDeviceState_t state,
    const deviceDesc_t in_deviceRequirements,
    deviceDesc_t* out_foundDevicePtr,
    const unsigned int devicesArraySize,
    unsigned int *out_amountOfFoundDevices);

xLinkPlatformErrorCode_t XLinkPlatformBootRemote(const deviceDesc_t* deviceDesc, const char* binaryPath);
xLinkPlatformErrorCode_t XLinkPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length);
xLinkPlatformErrorCode_t XLinkPlatformConnect(const char* devPathRead, const char* devPathWrite,
                         XLinkProtocol_t protocol, void** fd);
xLinkPlatformErrorCode_t XLinkPlatformBootBootloader(const char* name, XLinkProtocol_t protocol);

UsbSpeed_t get_usb_speed();
const char* get_mx_serial();
#endif // __DEVICE__

xLinkPlatformErrorCode_t XLinkPlatformCloseRemote(xLinkDeviceHandle_t* deviceHandle);

// ------------------------------------
// Device management. End.
// ------------------------------------



// ------------------------------------
// Data management. Begin.
// ------------------------------------

int XLinkPlatformWrite(xLinkDeviceHandle_t *deviceHandle, void *data, int size);
int XLinkPlatformRead(xLinkDeviceHandle_t *deviceHandle, void *data, int size);

void* XLinkPlatformAllocateData(uint32_t size, uint32_t alignment);
void XLinkPlatformDeallocateData(void *ptr, uint32_t size, uint32_t alignment);

// ------------------------------------
// Data management. End.
// ------------------------------------



// ------------------------------------
// Helpers. Begin.
// ------------------------------------

#ifndef __DEVICE__

int XLinkPlatformIsDescriptionValid(const deviceDesc_t *in_deviceDesc, const XLinkDeviceState_t state);
char* XLinkPlatformErrorToStr(const xLinkPlatformErrorCode_t errorCode);

// for deprecated API
XLinkPlatform_t XLinkPlatformPidToPlatform(const int pid);
XLinkDeviceState_t XLinkPlatformPidToState(const int pid);
// for deprecated API

#endif // __DEVICE__

// ------------------------------------
// Helpers. End.
// ------------------------------------

#ifdef __cplusplus
}
#endif

#endif

/* end of include file */
