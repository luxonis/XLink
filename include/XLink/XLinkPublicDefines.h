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
#include <stdbool.h>
#include "XLinkTime.h"
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
    X_LINK_USB_SPEED_UNKNOWN = 0,
    X_LINK_USB_SPEED_LOW,
    X_LINK_USB_SPEED_FULL,
    X_LINK_USB_SPEED_HIGH,
    X_LINK_USB_SPEED_SUPER,
    X_LINK_USB_SPEED_SUPER_PLUS
} UsbSpeed_t;

typedef enum{
    X_LINK_SUCCESS = 0,
    X_LINK_ALREADY_OPEN,
    X_LINK_COMMUNICATION_NOT_OPEN,
    X_LINK_COMMUNICATION_FAIL,
    X_LINK_COMMUNICATION_UNKNOWN_ERROR,
    X_LINK_DEVICE_NOT_FOUND,
    X_LINK_TIMEOUT,
    X_LINK_ERROR,
    X_LINK_OUT_OF_MEMORY,
    X_LINK_INSUFFICIENT_PERMISSIONS,
    X_LINK_DEVICE_ALREADY_IN_USE,
    X_LINK_NOT_IMPLEMENTED,
    X_LINK_INIT_USB_ERROR,
    X_LINK_INIT_TCP_IP_ERROR,
    X_LINK_INIT_PCIE_ERROR,
} XLinkError_t;

typedef enum{
    X_LINK_USB_VSC = 0,
    X_LINK_USB_CDC,
    X_LINK_PCIE,
    X_LINK_IPC,
    X_LINK_TCP_IP,
    X_LINK_NMB_OF_PROTOCOLS,
    X_LINK_ANY_PROTOCOL
} XLinkProtocol_t;

typedef enum{
    X_LINK_ANY_PLATFORM = 0,
    X_LINK_MYRIAD_2 = 2450,
    X_LINK_MYRIAD_X = 2480,
} XLinkPlatform_t;

typedef enum{
    /**
     * Used only for searching devices. It means that the device state is not important.
     */
    X_LINK_ANY_STATE = 0,
    /**
     * The device is booted (firmware is loaded) and the pipeline is running.
     */
    X_LINK_BOOTED,
    /**
     * Device isn't booted, e.g. for USB devices with no bootloader flashed. In such case it's waiting for the USB boot.
     */
    X_LINK_UNBOOTED,
    /**
     * The device is in bootloader and waiting for a connection. After the connection the state will change to BOOTED.
     */
    X_LINK_BOOTLOADER,
    /**
     * The device has booted the flashed firmware/pipeline (e.g. in case of OAK POE devices in standalone mode).
     */
    X_LINK_FLASH_BOOTED,
    /**
     * The device has booted the flashed firmware/pipeline (e.g. in case of OAK POE devices in standalone mode).
     */
    X_LINK_BOOTED_NON_EXCLUSIVE = X_LINK_FLASH_BOOTED,
} XLinkDeviceState_t;

typedef enum{
    X_LINK_PCIE_UNKNOWN_BOOTLOADER = 0,
    X_LINK_PCIE_SIMPLIFIED_BOOTLOADER = 1,
    X_LINK_PCIE_UNIFIED_BOOTLOADER = 2
} XLinkPCIEBootloader;

#define INVALID_STREAM_ID 0xDEADDEAD
#define INVALID_STREAM_ID_OUT_OF_MEMORY 0xDEADFFFF
#define INVALID_LINK_ID   0xFF
#define MAX_STREAM_NAME_LENGTH 52

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
    XLinkTimespec tRemoteSent; /// remote timestamp of when the packet was sent. Related to remote clock. Note: not directly related to local clock
    XLinkTimespec tReceived; /// local timestamp of when the packet was received. Related to local monotonic clock
} streamPacketDesc_t;

typedef struct XLinkProf_t
{
    float totalReadTime;
    float totalWriteTime;
    uint64_t totalReadBytes;
    uint64_t totalWriteBytes;
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
