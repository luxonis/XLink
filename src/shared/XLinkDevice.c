// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <errno.h>
#include "stdio.h"
#include "stdint.h"
#include "string.h"
#include "stdlib.h"

#include "XLink.h"
#include "XLinkErrorUtils.h"

#include "XLinkPlatform.h"
#include "XLinkPrivateFields.h"
#include "XLinkDispatcherImpl.h"

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif
#include "XLinkLog.h"
#include "XLinkStringUtils.h"

#define MAX_PATH_LENGTH (255)

#if (defined(_WIN32) || defined(_WIN64))
#include "win_time.h"
#endif

// ------------------------------------
// Global fields. Begin.
// ------------------------------------

XLinkGlobalHandler_t* glHandler; //TODO need to either protect this with semaphor
                                 //or make profiling data per device

xLinkDesc_t availableXLinks[MAX_LINKS];
pthread_mutex_t availableXLinksMutex = PTHREAD_MUTEX_INITIALIZER;
sem_t  pingSem; //to b used by myriad
DispatcherControlFunctions controlFunctionTbl;
linkId_t nextUniqueLinkId = 0; //incremental number, doesn't get decremented.

// ------------------------------------
// Global fields. End.
// ------------------------------------


static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t init_once = 0;

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static linkId_t getNextAvailableLinkUniqueId();
static xLinkDesc_t* getNextAvailableLink();
static void freeGivenLink(xLinkDesc_t* link);

#ifndef __DEVICE__

static XLinkError_t parsePlatformError(xLinkPlatformErrorCode_t rc);

#endif // __DEVICE__

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------



// ------------------------------------
// API implementation. Begin.
// ------------------------------------

XLinkError_t XLinkInitialize(XLinkGlobalHandler_t* globalHandler)
{
    XLINK_RET_IF(globalHandler == NULL);
    XLINK_RET_ERR_IF(pthread_mutex_lock(&init_mutex), X_LINK_ERROR);
    if(init_once){
        pthread_mutex_unlock(&init_mutex);
        return X_LINK_SUCCESS;
    }

#ifdef __DEVICE__
    mvLogLevelSet(MVLOG_FATAL);
    mvLogDefaultLevelSet(MVLOG_FATAL);
#endif

    ASSERT_XLINK(XLINK_MAX_STREAMS <= MAX_POOLS_ALLOC);
    glHandler = globalHandler;
    if (sem_init(&pingSem,0,0)) {
        mvLog(MVLOG_ERROR, "Can't create semaphore\n");
    }
    int i;

    xLinkPlatformErrorCode_t init_status = XLinkPlatformInit(globalHandler->options);
    if (init_status != X_LINK_PLATFORM_SUCCESS) {
        pthread_mutex_unlock(&init_mutex);
        return parsePlatformError(init_status);
    }

    //Using deprecated fields. Begin.
    int loglevel = globalHandler->loglevel;
    int protocol = globalHandler->protocol;
    //Using deprecated fields. End.

    memset((void*)globalHandler, 0, sizeof(XLinkGlobalHandler_t));

    //Using deprecated fields. Begin.
    globalHandler->loglevel = loglevel;
    globalHandler->protocol = protocol;
    //Using deprecated fields. End.

    controlFunctionTbl.eventReceive      = &dispatcherEventReceive;
    controlFunctionTbl.eventSend         = &dispatcherEventSend;
    controlFunctionTbl.localGetResponse  = &dispatcherLocalEventGetResponse;
    controlFunctionTbl.remoteGetResponse = &dispatcherRemoteEventGetResponse;
    controlFunctionTbl.closeLink         = &dispatcherCloseLink;
    controlFunctionTbl.closeDeviceFd     = &dispatcherCloseDeviceFd;

    if (DispatcherInitialize(&controlFunctionTbl)) {
        mvLog(MVLOG_ERROR, "Condition failed: DispatcherInitialize(&controlFunctionTbl)");
        pthread_mutex_unlock(&init_mutex);
        return X_LINK_ERROR;
    }

    //initialize availableStreams
    memset(availableXLinks, 0, sizeof(availableXLinks));

    xLinkDesc_t* link;
    for (i = 0; i < MAX_LINKS; i++) {
        link = &availableXLinks[i];

        link->id = INVALID_LINK_ID;
        link->deviceHandle.xLinkFD = NULL;
        link->peerState = XLINK_NOT_INIT;
        int stream;
        for (stream = 0; stream < XLINK_MAX_STREAMS; stream++)
            link->availableStreams[stream].id = INVALID_STREAM_ID;
    }

#ifdef __DEVICE__
    link = getNextAvailableLink();
    if (link == NULL) {
        pthread_mutex_unlock(&init_mutex);
        return X_LINK_COMMUNICATION_NOT_OPEN;
    }
    link->peerState = XLINK_UP;
    link->deviceHandle.xLinkFD = NULL;
    link->deviceHandle.protocol = globalHandler->protocol;

    xLinkDeviceHandle_t temp = {0};
    temp.protocol = globalHandler->protocol;
    {
        int rc;
        if (rc = DispatcherStart(&temp)) { //myriad has one
            mvLog(MVLOG_ERROR, " DispatcherStart(&temp) method call failed with an error: %d", rc);
            pthread_mutex_unlock(&init_mutex);
            return rc;
        }
    }

    while(((sem_wait(&pingSem) == -1) && errno == EINTR)
        continue;

#endif

    init_once = 1;
    int status = pthread_mutex_unlock(&init_mutex);
    if(status){
        // rare and unstable scenario; xlink is technically initialized yet mutex unlock failed
        return X_LINK_ERROR;
    }

    return X_LINK_SUCCESS;
}

#ifndef __DEVICE__

int XLinkIsDescriptionValid(const deviceDesc_t *in_deviceDesc, const XLinkDeviceState_t state) {
    return XLinkPlatformIsDescriptionValid(in_deviceDesc, state);
}

XLinkError_t XLinkFindFirstSuitableDevice(const deviceDesc_t in_deviceRequirements, deviceDesc_t *out_foundDevice)
{
    XLINK_RET_IF(out_foundDevice == NULL);

    xLinkPlatformErrorCode_t rc;
    unsigned numFoundDevices = 0;
    rc = XLinkPlatformFindDevices(in_deviceRequirements, out_foundDevice, 1, &numFoundDevices);
    if(numFoundDevices <= 0){
        return X_LINK_DEVICE_NOT_FOUND;
    }
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkFindAllSuitableDevices(const deviceDesc_t in_deviceRequirements,
                                         deviceDesc_t *out_foundDevicesPtr,
                                         const unsigned int devicesArraySize,
                                         unsigned int* out_foundDevicesCount) {
    XLINK_RET_IF(out_foundDevicesPtr == NULL);
    XLINK_RET_IF(devicesArraySize <= 0);
    XLINK_RET_IF(out_foundDevicesCount == NULL);

    xLinkPlatformErrorCode_t rc;
    rc = XLinkPlatformFindDevices(in_deviceRequirements, out_foundDevicesPtr, devicesArraySize, out_foundDevicesCount);

    return parsePlatformError(rc);
}

//Called only from app - per device
XLinkError_t XLinkConnect(XLinkHandler_t* handler)
{
    XLINK_RET_IF(handler == NULL);
    if (strnlen(handler->devicePath, MAX_PATH_LENGTH) < 2) {
        mvLog(MVLOG_ERROR, "Device path is incorrect");
        return X_LINK_ERROR;
    }

    xLinkDesc_t* link = getNextAvailableLink();
    XLINK_RET_IF(link == NULL);
    mvLog(MVLOG_DEBUG,"%s() device name %s glHandler %p protocol %d\n", __func__, handler->devicePath, glHandler, handler->protocol);

    link->deviceHandle.protocol = handler->protocol;
    int connectStatus = XLinkPlatformConnect(handler->devicePath2, handler->devicePath,
                                             link->deviceHandle.protocol, &link->deviceHandle.xLinkFD);

    if (connectStatus < 0) {
        /**
         * Connection may be unsuccessful at some amount of first tries.
         * In this case, asserting the status provides enormous amount of logs in tests.
         */

        // Free used link
        freeGivenLink(link);

        // Return an informative error
        return parsePlatformError(connectStatus);
    }

    XLINK_RET_ERR_IF(
        DispatcherStart(&link->deviceHandle) != X_LINK_SUCCESS, X_LINK_TIMEOUT);

    xLinkEvent_t event = {0};

    event.header.type = XLINK_PING_REQ;
    event.deviceHandle = link->deviceHandle;
    DispatcherAddEvent(EVENT_LOCAL, &event);

    if (DispatcherWaitEventComplete(&link->deviceHandle, XLINK_NO_RW_TIMEOUT)) {
        DispatcherClean(&link->deviceHandle);
        return X_LINK_TIMEOUT;
    }

    link->peerState = XLINK_UP;
    #if (!defined(_WIN32) && !defined(_WIN64) )
        link->usbConnSpeed = get_usb_speed();
        mv_strcpy(link->mxSerialId, XLINK_MAX_MX_ID_SIZE, get_mx_serial());
    #else
        link->usbConnSpeed = X_LINK_USB_SPEED_UNKNOWN;
        mv_strcpy(link->mxSerialId, XLINK_MAX_MX_ID_SIZE, "UNKNOWN");
    #endif

    link->hostClosedFD = 0;
    handler->linkId = link->id;
    return X_LINK_SUCCESS;
}


//Called only from app - per device
XLinkError_t XLinkBootBootloader(const deviceDesc_t* deviceDesc)
{
    return parsePlatformError(XLinkPlatformBootBootloader(deviceDesc->name, deviceDesc->protocol));
}

XLinkError_t XLinkBootMemory(const deviceDesc_t* deviceDesc, const uint8_t* buffer, unsigned long size)
{
    if (XLinkPlatformBootFirmware(deviceDesc, (const char*) buffer, size) == 0) {
        return X_LINK_SUCCESS;
    }

    return X_LINK_COMMUNICATION_FAIL;
}

XLinkError_t XLinkBoot(const deviceDesc_t* deviceDesc, const char* binaryPath)
{
    if (XLinkPlatformBootRemote(deviceDesc, binaryPath) == 0) {
        return X_LINK_SUCCESS;
    }

    return X_LINK_COMMUNICATION_FAIL;
}

XLinkError_t XLinkBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, unsigned long length) {
    if (!XLinkPlatformBootFirmware(deviceDesc, firmware, length)) {
        return X_LINK_SUCCESS;
    }

    return X_LINK_COMMUNICATION_FAIL;
}

XLinkError_t XLinkResetRemote(linkId_t id)
{
    xLinkDesc_t* link = getLinkById(id);
    XLINK_RET_IF(link == NULL);

    if (getXLinkState(link) != XLINK_UP) {
        mvLog(MVLOG_WARN, "Link is down, close connection to device without reset");
        XLinkPlatformCloseRemote(&link->deviceHandle);
        return X_LINK_COMMUNICATION_NOT_OPEN;
    }

    // Add event to reset device. After sending it, dispatcher will close fd link
    xLinkEvent_t event = {0};
    event.header.type = XLINK_RESET_REQ;
    event.deviceHandle = link->deviceHandle;
    mvLog(MVLOG_DEBUG, "sending reset remote event\n");
    DispatcherAddEvent(EVENT_LOCAL, &event);
    XLINK_RET_ERR_IF(DispatcherWaitEventComplete(&link->deviceHandle, XLINK_NO_RW_TIMEOUT),
        X_LINK_TIMEOUT);

    int rc;
    while(((rc = XLink_sem_wait(&link->dispatcherClosedSem)) == -1) && errno == EINTR)
        continue;
    if(rc) {
        mvLog(MVLOG_ERROR,"can't wait dispatcherClosedSem\n");
        return X_LINK_ERROR;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkResetRemoteTimeout(linkId_t id, int timeoutMs)
{
    xLinkDesc_t* link = getLinkById(id);
    XLINK_RET_IF(link == NULL);

    if (getXLinkState(link) != XLINK_UP) {
        mvLog(MVLOG_WARN, "Link is down, close connection to device without reset");
        XLinkPlatformCloseRemote(&link->deviceHandle);
        return X_LINK_COMMUNICATION_NOT_OPEN;
    }

    // Add event to reset device. After sending it, dispatcher will close fd link
    xLinkEvent_t event = {0};
    event.header.type = XLINK_RESET_REQ;
    event.deviceHandle = link->deviceHandle;
    mvLog(MVLOG_DEBUG, "sending reset remote event\n");

    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);

    struct timespec absTimeout = start;
    int64_t sec = timeoutMs / 1000;
    absTimeout.tv_sec += sec;
    absTimeout.tv_nsec += (long)((timeoutMs - (sec * 1000)) * 1000000);
    int64_t secOver = absTimeout.tv_nsec / 1000000000;
    absTimeout.tv_nsec -= (long)(secOver * 1000000000);
    absTimeout.tv_sec += secOver;

    xLinkEvent_t* ev = DispatcherAddEvent(EVENT_LOCAL, &event);
    if(ev == NULL) {
        mvLog(MVLOG_ERROR, "Dispatcher failed on adding event. type: %s, id: %d, stream name: %s\n",
            TypeToStr(event.header.type), event.header.id, event.header.streamName);
        return X_LINK_ERROR;
    }

    XLinkError_t ret = DispatcherWaitEventCompleteTimeout(&link->deviceHandle, absTimeout);

    if(ret != X_LINK_SUCCESS){
        // Closing device link unblocks any blocked events
        // Afterwards the dispatcher can properly cleanup in its own thread
        DispatcherDeviceFdDown(&link->deviceHandle);
    }

    // Wait for dispatcher to be closed
    if(XLink_sem_wait(&link->dispatcherClosedSem)) {
        mvLog(MVLOG_ERROR,"can't wait dispatcherClosedSem\n");
        return X_LINK_ERROR;
    }

    return ret;

}

XLinkError_t XLinkResetAll()
{
#if defined(NO_BOOT)
    mvLog(MVLOG_INFO, "Devices will not be restarted for this configuration (NO_BOOT)");
#else
    int i;
    for (i = 0; i < MAX_LINKS; i++) {
        if (availableXLinks[i].id != INVALID_LINK_ID) {
            xLinkDesc_t* link = &availableXLinks[i];
            int stream;
            for (stream = 0; stream < XLINK_MAX_STREAMS; stream++) {
                if (link->availableStreams[stream].id != INVALID_STREAM_ID) {
                    streamId_t streamId = link->availableStreams[stream].id;
                    mvLog(MVLOG_DEBUG,"%s() Closing stream (stream = %d) %d on link %d\n",
                          __func__, stream, (int) streamId, (int) link->id);
                    COMBINE_IDS(streamId, link->id);
                    if (XLinkCloseStream(streamId) != X_LINK_SUCCESS) {
                        mvLog(MVLOG_WARN,"Failed to close stream");
                    }
                }
            }
            if (XLinkResetRemote(link->id) != X_LINK_SUCCESS) {
                mvLog(MVLOG_WARN,"Failed to reset");
            }
        }
    }
#endif
    return X_LINK_SUCCESS;
}

#endif // __DEVICE__

XLinkError_t XLinkProfStart()
{
    glHandler->profEnable = 1;
    glHandler->profilingData.totalReadBytes = 0;
    glHandler->profilingData.totalWriteBytes = 0;
    glHandler->profilingData.totalWriteTime = 0;
    glHandler->profilingData.totalReadTime = 0;
    glHandler->profilingData.totalBootCount = 0;
    glHandler->profilingData.totalBootTime = 0;

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkProfStop()
{
    glHandler->profEnable = 0;
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkProfPrint()
{
    printf("XLink profiling results:\n");
    if (glHandler->profilingData.totalWriteTime)
    {
        printf("Average write speed: %f MB/Sec\n",
               glHandler->profilingData.totalWriteBytes /
               glHandler->profilingData.totalWriteTime /
               1024.0 /
               1024.0 );
    }
    if (glHandler->profilingData.totalReadTime)
    {
        printf("Average read speed: %f MB/Sec\n",
               glHandler->profilingData.totalReadBytes /
               glHandler->profilingData.totalReadTime /
               1024.0 /
               1024.0);
    }
    if (glHandler->profilingData.totalBootCount)
    {
        printf("Average boot speed: %f sec\n",
               glHandler->profilingData.totalBootTime /
               glHandler->profilingData.totalBootCount);
    }
    return X_LINK_SUCCESS;
}

UsbSpeed_t XLinkGetUSBSpeed(linkId_t id){
    xLinkDesc_t* link = getLinkById(id);
    return link->usbConnSpeed;
}

const char* XLinkGetMxSerial(linkId_t id){
    xLinkDesc_t* link = getLinkById(id);
    return link->mxSerialId;
}

// ------------------------------------
// API implementation. End.
// ------------------------------------


// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------

// Used only by getNextAvailableLink
static linkId_t getNextAvailableLinkUniqueId()
{
    linkId_t start = nextUniqueLinkId;
    do
    {
        int i;
        for (i = 0; i < MAX_LINKS; i++)
        {
            if (availableXLinks[i].id != INVALID_LINK_ID &&
                availableXLinks[i].id == nextUniqueLinkId)
                break;
        }
        if (i >= MAX_LINKS)
        {
            return nextUniqueLinkId;
        }
        nextUniqueLinkId++;
        if (nextUniqueLinkId == INVALID_LINK_ID)
        {
            nextUniqueLinkId = 0;
        }
    } while (start != nextUniqueLinkId);
    mvLog(MVLOG_ERROR, "%s():- no next available unique link id!\n", __func__);
    return INVALID_LINK_ID;
}

static xLinkDesc_t* getNextAvailableLink() {

    XLINK_RET_ERR_IF(pthread_mutex_lock(&availableXLinksMutex) != 0, NULL);

    linkId_t id = getNextAvailableLinkUniqueId();
    if(id == INVALID_LINK_ID){
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
        return NULL;
    }

    int i;
    for (i = 0; i < MAX_LINKS; i++) {
        if (availableXLinks[i].id == INVALID_LINK_ID) {
            break;
        }
    }

    if(i >= MAX_LINKS) {
        mvLog(MVLOG_ERROR,"%s():- no next available link!\n", __func__);
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
        return NULL;
    }

    xLinkDesc_t* link = &availableXLinks[i];

    if (XLink_sem_init(&link->dispatcherClosedSem, 0 ,0)) {
        mvLog(MVLOG_ERROR, "Cannot initialize semaphore\n");
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
        return NULL;
    }

    link->id = id;
    XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);

    return link;
}

static void freeGivenLink(xLinkDesc_t* link) {

    if(pthread_mutex_lock(&availableXLinksMutex) != 0){
        mvLog(MVLOG_ERROR, "Cannot lock mutex\n");
        return;
    }

    link->id = INVALID_LINK_ID;
    if (XLink_sem_destroy(&link->dispatcherClosedSem)) {
        mvLog(MVLOG_ERROR, "Cannot destroy semaphore\n");
    }

    pthread_mutex_unlock(&availableXLinksMutex);

}

#ifndef __DEVICE__

static XLinkError_t parsePlatformError(xLinkPlatformErrorCode_t rc) {
    switch (rc) {
        case X_LINK_PLATFORM_SUCCESS:
            return X_LINK_SUCCESS;
        case X_LINK_PLATFORM_DEVICE_NOT_FOUND:
            return X_LINK_DEVICE_NOT_FOUND;
        case X_LINK_PLATFORM_TIMEOUT:
            return X_LINK_TIMEOUT;
        case X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS:
            return X_LINK_INSUFFICIENT_PERMISSIONS;
        case X_LINK_PLATFORM_DEVICE_BUSY:
            return X_LINK_DEVICE_ALREADY_IN_USE;
        case X_LINK_PLATFORM_USB_DRIVER_NOT_LOADED:
            return X_LINK_INIT_USB_ERROR;
        case X_LINK_PLATFORM_TCP_IP_DRIVER_NOT_LOADED:
            return X_LINK_INIT_TCP_IP_ERROR;
        case X_LINK_PLATFORM_PCIE_DRIVER_NOT_LOADED:
            return X_LINK_INIT_PCIE_ERROR;
        case X_LINK_PLATFORM_ERROR:
        case X_LINK_PLATFORM_INVALID_PARAMETERS:
        default:
            return X_LINK_ERROR;
    }
}

#endif // __DEVICE__

/**
 * @brief Returns enum string value
 * @return Pointer to null terminated string
 */
const char* XLinkErrorToStr(XLinkError_t val) {
    switch (val) {
        case X_LINK_SUCCESS: return "X_LINK_SUCCESS";
        case X_LINK_ALREADY_OPEN: return "X_LINK_ALREADY_OPEN";
        case X_LINK_COMMUNICATION_NOT_OPEN: return "X_LINK_COMMUNICATION_NOT_OPEN";
        case X_LINK_COMMUNICATION_FAIL: return "X_LINK_COMMUNICATION_FAIL";
        case X_LINK_COMMUNICATION_UNKNOWN_ERROR: return "X_LINK_COMMUNICATION_UNKNOWN_ERROR";
        case X_LINK_DEVICE_NOT_FOUND: return "X_LINK_DEVICE_NOT_FOUND";
        case X_LINK_TIMEOUT: return "X_LINK_TIMEOUT";
        case X_LINK_ERROR: return "X_LINK_ERROR";
        case X_LINK_OUT_OF_MEMORY: return "X_LINK_OUT_OF_MEMORY";
        case X_LINK_INSUFFICIENT_PERMISSIONS: return "X_LINK_INSUFFICIENT_PERMISSIONS";
        case X_LINK_DEVICE_ALREADY_IN_USE: return "X_LINK_DEVICE_ALREADY_IN_USE";
        case X_LINK_NOT_IMPLEMENTED: return "X_LINK_NOT_IMPLEMENTED";
        case X_LINK_INIT_USB_ERROR: return "X_LINK_INIT_USB_ERROR";
        case X_LINK_INIT_TCP_IP_ERROR: return "X_LINK_INIT_TCP_IP_ERROR";
        case X_LINK_INIT_PCIE_ERROR: return "X_LINK_INIT_PCIE_ERROR";
        default:
            return "INVALID_ENUM_VALUE";
            break;
    }
}

/**
 * @brief Returns enum string value
 * @return Pointer to null terminated string
 */
const char* XLinkProtocolToStr(XLinkProtocol_t val) {
    switch (val) {
        case X_LINK_USB_VSC: return "X_LINK_USB_VSC";
        case X_LINK_USB_CDC: return "X_LINK_USB_CDC";
        case X_LINK_PCIE: return "X_LINK_PCIE";
        case X_LINK_IPC: return "X_LINK_IPC";
        case X_LINK_TCP_IP: return "X_LINK_TCP_IP";
        case X_LINK_NMB_OF_PROTOCOLS: return "X_LINK_NMB_OF_PROTOCOLS";
        case X_LINK_ANY_PROTOCOL: return "X_LINK_ANY_PROTOCOL";
        default:
            return "INVALID_ENUM_VALUE";
            break;
    }
}

/**
 * @brief Returns enum string value
 * @return Pointer to null terminated string
 */
const char* XLinkPlatformToStr(XLinkPlatform_t val) {
    switch (val) {
        case X_LINK_ANY_PLATFORM: return "X_LINK_ANY_PLATFORM";
        case X_LINK_MYRIAD_2: return "X_LINK_MYRIAD_2";
        case X_LINK_MYRIAD_X: return "X_LINK_MYRIAD_X";
        default:
            return "INVALID_ENUM_VALUE";
            break;
    }
}

/**
 * @brief Returns enum string value
 * @return Pointer to null terminated string
 */
const char* XLinkDeviceStateToStr(XLinkDeviceState_t val) {
    switch (val) {
        case X_LINK_ANY_STATE: return "X_LINK_ANY_STATE";
        case X_LINK_BOOTED: return "X_LINK_BOOTED";
        case X_LINK_UNBOOTED: return "X_LINK_UNBOOTED";
        case X_LINK_BOOTLOADER: return "X_LINK_BOOTLOADER";
        case X_LINK_BOOTED_NON_EXCLUSIVE: return "X_LINK_BOOTED_NON_EXCLUSIVE";
        default:
            return "INVALID_ENUM_VALUE";
            break;
    }
}


/**
 * @brief Returns enum string value
 * @return Pointer to null terminated string
 */
const char* XLinkPCIEBootloaderToStr(XLinkPCIEBootloader val) {
    switch (val) {
        case X_LINK_PCIE_UNKNOWN_BOOTLOADER: return "X_LINK_PCIE_UNKNOWN_BOOTLOADER";
        case X_LINK_PCIE_SIMPLIFIED_BOOTLOADER: return "X_LINK_PCIE_SIMPLIFIED_BOOTLOADER";
        case X_LINK_PCIE_UNIFIED_BOOTLOADER: return "X_LINK_PCIE_UNIFIED_BOOTLOADER";
        default:
            return "INVALID_ENUM_VALUE";
            break;
    }
}

// ------------------------------------
// Helpers implementation. End.
// ------------------------------------