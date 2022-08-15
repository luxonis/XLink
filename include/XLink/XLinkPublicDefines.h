// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///
/// @file
///
/// @brief     Application configuration Leon header
///
#ifndef _XLINKPUBLICDEFINES_H
#define _XLINKPUBLICDEFINES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C"
{
#endif

#define XLINK_MAX_MX_ID_SIZE 32
#define XLINK_MAX_NAME_SIZE 64

#ifdef XLINK_MAX_STREAM_RES
#define XLINK_MAX_STREAMS XLINK_MAX_STREAM_RES
#else
#define XLINK_MAX_STREAMS 32
#endif
#define XLINK_MAX_PACKETS_PER_STREAM 64
#define XLINK_NO_RW_TIMEOUT 0xFFFFFFFF


typedef enum {
    XLINK_USB_SPEED_UNKNOWN = 0,
    XLINK_USB_SPEED_LOW,
    XLINK_USB_SPEED_FULL,
    XLINK_USB_SPEED_HIGH,
    XLINK_USB_SPEED_SUPER,
    XLINK_USB_SPEED_SUPER_PLUS
} UsbSpeed_t;

typedef enum{
    XLINK_SUCCESS = 0,
    XLINK_ALREADY_OPEN,
    XLINK_COMMUNICATION_NOT_OPEN,
    XLINK_COMMUNICATION_FAIL,
    XLINK_COMMUNICATION_UNKNOWN_ERROR,
    XLINK_DEVICE_NOT_FOUND,
    XLINK_TIMEOUT,
    XLINK_ERROR,
    XLINK_OUT_OF_MEMORY,
    XLINK_INSUFFICIENT_PERMISSIONS,
    XLINK_DEVICE_ALREADY_IN_USE,
    XLINK_NOT_IMPLEMENTED,
    XLINK_INIT_USB_ERROR,
    XLINK_INIT_TCP_IP_ERROR,
    XLINK_INIT_PCIE_ERROR,
} XLinkError_t;

typedef enum{
    XLINK_USB_VSC = 0,
    XLINK_USB_CDC,
    XLINK_PCIE,
    XLINK_IPC,
    XLINK_TCP_IP,
    XLINK_NMB_OF_PROTOCOLS,
    XLINK_ANY_PROTOCOL
} XLinkProtocol_t;

typedef enum{
    XLINK_ANY_PLATFORM = 0,
    XLINK_MYRIAD_2 = 2450,
    XLINK_MYRIAD_X = 2480,
    XLINK_KEEMBAY = 3000,
} XLinkPlatform_t;

typedef enum{
    XLINK_ANY_STATE = 0,
    XLINK_BOOTED,
    XLINK_UNBOOTED,
    XLINK_BOOTLOADER,
    XLINK_FLASH_BOOTED,
    XLINK_GATE,
} XLinkDeviceState_t;

typedef enum{
    XLINK_PCIE_UNKNOWN_BOOTLOADER = 0,
    XLINK_PCIE_SIMPLIFIED_BOOTLOADER = 1,
    XLINK_PCIE_UNIFIED_BOOTLOADER = 2
} XLinkPCIEBootloader;

#define INVALID_STREAM_ID 0xDEADDEAD
#define INVALID_STREAM_ID_OUT_OF_MEMORY 0xDEADFFFF
#define INVALID_LINK_ID   0xFF
#define MAX_STREAM_NAME_LENGTH 64

typedef uint32_t streamId_t;
typedef uint8_t linkId_t;

typedef struct {
    XLinkProtocol_t protocol;
    XLinkPlatform_t platform;
    char name[XLINK_MAX_NAME_SIZE];
    XLinkDeviceState_t state;
    char mxid[XLINK_MAX_MX_ID_SIZE];
    XLinkError_t status;
    bool nameHintOnly;
} deviceDesc_t;

typedef struct streamPacketDesc_t
{
    uint8_t* data;
    uint32_t length;
} streamPacketDesc_t;

typedef struct XLinkProf_t
{
    float totalReadTime;
    float totalWriteTime;
    unsigned long totalReadBytes;
    unsigned long totalWriteBytes;
    unsigned long totalBootCount;
    float totalBootTime;
} XLinkProf_t;

typedef struct XLinkGlobalHandler_t
{
    int profEnable;
    XLinkProf_t profilingData;
    void* options;

    //Deprecated fields. Begin.
    int loglevel;
    int protocol;
    //Deprecated fields. End.
} XLinkGlobalHandler_t;

typedef struct
{
    char* devicePath;
    char* devicePath2;
    int linkId;
    XLinkProtocol_t protocol;
} XLinkHandler_t;

//Deprecated defines. Begin.

typedef enum{
    USB_VSC = 0,
    USB_CDC,
    PCIE,
    IPC,
    NMB_OF_PROTOCOLS
} XLinkProtocol_deprecated_t;

//Deprecated defines. End.

#ifdef __cplusplus
}
#endif

#endif

/* end of include file */
