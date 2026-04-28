// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <errno.h>
#include "XLinkPublicDefines.h"
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

#include "tcpip_host.h"

DispatcherControlFunctions controlFunctionTbl;


static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t init_once = 0;
static pthread_mutex_t link_id_mutex = PTHREAD_MUTEX_INITIALIZER;
static linkId_t nextLinkId = 0;

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static xLinkDesc_t* allocateSession(XLinkHandler_t* handler);
static void deallocateSession(XLinkHandler_t* handler);
static XLinkError_t parsePlatformError(xLinkPlatformErrorCode_t rc);
static XLinkError_t stopAndReapSession(XLinkHandler_t* handler, XLinkError_t status);

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

    ASSERT_XLINK(XLINK_MAX_STREAMS <= MAX_POOLS_ALLOC);
    XLinkGlobalHandlerAssign(globalHandler);
    int i;

    xLinkPlatformErrorCode_t init_status = XLinkPlatformInit(globalHandler);
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

    init_once = 1;
    int status = pthread_mutex_unlock(&init_mutex);
    if(status){
        // rare and unstable scenario; xlink is technically initialized yet mutex unlock failed
        return X_LINK_ERROR;
    }

    return X_LINK_SUCCESS;
}

void XLinkDiscoveryServiceSetCallbackReset(void (*cb)()) {
    tcpip_set_discovery_service_reset_callback(cb);
}
XLinkError_t XLinkDiscoveryServiceStart(const char* deviceId, XLinkDeviceState_t state, XLinkPlatform_t platform) {
    return parsePlatformError(tcpip_start_discovery_service(deviceId, state, platform));
}
bool XLinkDiscoveryServiceIsRunning() {
    return tcpip_is_running_discovery_service();
}
void XLinkDiscoveryServiceStop() {
    tcpip_stop_discovery_service();
}
void XLinkDiscoveryServiceDetach() {
    tcpip_detach_discovery_service();
}

XLinkError_t XLinkServer(XLinkHandler_t* handler, const char* deviceId, XLinkDeviceState_t state, XLinkPlatform_t platform) {
    // Start discovery
    XLinkError_t ret = XLinkDiscoveryServiceStart(deviceId, state, platform);
    if(ret != X_LINK_SUCCESS)  {
        return ret;
    }

    // Detach discovery
    XLinkDiscoveryServiceDetach();

    // Start server and return
    return XLinkServerOnly(handler);
}


XLinkError_t XLinkServerOnly(XLinkHandler_t* handler)
{
    XLINK_RET_IF(handler == NULL);
    if (strnlen(handler->devicePath, MAX_PATH_LENGTH) < 2) {
        mvLog(MVLOG_ERROR, "Device path is incorrect");
        return X_LINK_ERROR;
    }

    xLinkDesc_t* link = allocateSession(handler);
    XLINK_RET_IF(link == NULL);
    mvLog(MVLOG_DEBUG,"%s() device name %s link %p protocol %d\n", __func__, handler->devicePath, link, handler->protocol);

    link->deviceHandle.protocol = handler->protocol;
    int connectStatus = XLinkPlatformServer(handler->devicePath2, handler->devicePath,
                                             &link->deviceHandle.protocol, &link->deviceHandle.xLinkFD);

    if (connectStatus < 0) {
        /**
         * Connection may be unsuccessful at some amount of first tries.
         * In this case, asserting the status provides enormous amount of logs in tests.
         */

        // Free used link
        deallocateSession(handler);

        // Return an informative error
        return parsePlatformError(connectStatus);
    }

    if (DispatcherStartServer(link) != X_LINK_SUCCESS) {
        deallocateSession(handler);
        return X_LINK_TIMEOUT;
    }

    // Wait till client pings
    if (XLink_sem_wait(&link->pingSem)) {
        deallocateSession(handler);
        return X_LINK_ERROR;
    }

    link->peerState = XLINK_UP;
    XLinkSessionLifecycleMarkRunning(getSession(handler));
    link->hostClosedFD = 0;
    handler->linkId = link->id;
    return X_LINK_SUCCESS;
}

int XLinkIsDescriptionValid(const deviceDesc_t *in_deviceDesc, const XLinkDeviceState_t state) {
    return XLinkPlatformIsDescriptionValid(in_deviceDesc, state);
}

XLinkError_t XLinkFindFirstSuitableDevice(const deviceDesc_t in_deviceRequirements, deviceDesc_t *out_foundDevice)
{
    XLINK_RET_IF(out_foundDevice == NULL);

    xLinkPlatformErrorCode_t rc;
    unsigned numFoundDevices = 0;
    rc = XLinkPlatformFindDevices(in_deviceRequirements, out_foundDevice, 1, &numFoundDevices, XLINK_DEVICE_DEFAULT_SEARCH_TIMEOUT_MS);
    if(numFoundDevices <= 0){
        return X_LINK_DEVICE_NOT_FOUND;
    }
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkFindAllSuitableDevices(const deviceDesc_t in_deviceRequirements,
                                         deviceDesc_t *out_foundDevicesPtr,
                                         const unsigned int devicesArraySize,
                                         unsigned int* out_foundDevicesCount,
                                         int timeoutMs) {
    XLINK_RET_IF(out_foundDevicesPtr == NULL);
    XLINK_RET_IF(devicesArraySize <= 0);
    XLINK_RET_IF(out_foundDevicesCount == NULL);

    xLinkPlatformErrorCode_t rc;
    rc = XLinkPlatformFindDevices(in_deviceRequirements, out_foundDevicesPtr, devicesArraySize, out_foundDevicesCount, timeoutMs);

    return parsePlatformError(rc);
}

XLinkError_t XLinkSearchForDevices(const deviceDesc_t in_deviceRequirements,
                                         deviceDesc_t *out_foundDevicesPtr,
                                         const unsigned int devicesArraySize,
                                         unsigned int* out_foundDevicesCount,
                                         int timeoutMs,
                                         bool (*cb)(deviceDesc_t*, unsigned int)) {
    XLINK_RET_IF(out_foundDevicesPtr == NULL);
    XLINK_RET_IF(devicesArraySize <= 0);
    XLINK_RET_IF(out_foundDevicesCount == NULL);

    xLinkPlatformErrorCode_t rc;
    rc = XLinkPlatformFindDevicesDynamic(in_deviceRequirements, out_foundDevicesPtr, devicesArraySize, out_foundDevicesCount, timeoutMs, cb);
    return parsePlatformError(rc);
}

XLinkError_t XLinkConnectTimeout(XLinkHandler_t* handler, unsigned int timeoutMs)
{
    XLINK_RET_IF(handler == NULL);
    if (strnlen(handler->devicePath, MAX_PATH_LENGTH) < 2) {
        mvLog(MVLOG_ERROR, "Device path is incorrect");
        return X_LINK_ERROR;
    }

    xLinkDesc_t* link = allocateSession(handler);
    XLINK_RET_IF(link == NULL);
    mvLog(MVLOG_DEBUG,"%s() device name %s link %p protocol %d\n", __func__, handler->devicePath, link, handler->protocol);

    link->deviceHandle.protocol = handler->protocol;
    int connectStatus = XLinkPlatformConnect(handler->devicePath2, handler->devicePath,
                                             &link->deviceHandle.protocol, &link->deviceHandle.xLinkFD);
    
    if (connectStatus < 0) {
        /**
         * Connection may be unsuccessful at some amount of first tries.
         * In this case, asserting the status provides enormous amount of logs in tests.
         */

        // Free used link
        deallocateSession(handler);

        // Return an informative error
        return parsePlatformError(connectStatus);
    }
    
    if (DispatcherStart(link) != X_LINK_SUCCESS) {
        deallocateSession(handler);
        return X_LINK_TIMEOUT;
    }

    xLinkEvent_t event = {0};

    event.header.type = XLINK_PING_REQ;
    event.deviceHandle = link->deviceHandle;
    if (DispatcherAddEvent(EVENT_LOCAL, &event) == NULL) {
        XLinkSessionLifecycleBeginStop(getSession(handler), X_LINK_LINK_DOWN_CONNECT_FAILURE);
        return stopAndReapSession(handler, X_LINK_TIMEOUT);
    }

    if (DispatcherWaitEventComplete(&link->deviceHandle, timeoutMs)) {
        XLinkSessionLifecycleBeginStop(getSession(handler), X_LINK_LINK_DOWN_CONNECT_FAILURE);
        return stopAndReapSession(handler, X_LINK_TIMEOUT);
    }

    link->peerState = XLINK_UP;
    XLinkSessionLifecycleMarkRunning(getSession(handler));
    link->hostClosedFD = 0;
    handler->linkId = link->id;
    return X_LINK_SUCCESS;
}

//Called only from app - per device
XLinkError_t XLinkConnect(XLinkHandler_t* handler)
{
    return XLinkConnectTimeout(handler, XLINK_NO_RW_TIMEOUT);
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

XLinkError_t XLinkResetRemoteTimeout(XLinkHandler_t* handler, unsigned int timeoutMs)
{
    XLinkSession_t* session = getSession(handler);
    XLINK_RET_IF(session == NULL);
    xLinkDesc_t* link = &session->link;

    if (XLinkSessionLifecycleGetState(session) != XLINK_SESSION_RUNNING || getXLinkState(link) != XLINK_UP) {
        mvLog(MVLOG_WARN, "Link is down, cleaning up local session without reset");
        return stopAndReapSession(handler, X_LINK_COMMUNICATION_NOT_OPEN);
    }

    // Add event to reset device. After sending it, dispatcher will close fd link
    xLinkEvent_t event = {0};
    event.header.type = XLINK_RESET_REQ;
    event.deviceHandle = link->deviceHandle;
    mvLog(MVLOG_DEBUG, "sending reset remote event\n");

    XLinkError_t ret = X_LINK_SUCCESS;
    if (DispatcherAddEvent(EVENT_LOCAL, &event) == NULL) {
        ret = X_LINK_COMMUNICATION_NOT_OPEN;
    } else if (DispatcherWaitEventComplete(&link->deviceHandle, timeoutMs)) {
        ret = (timeoutMs == XLINK_NO_RW_TIMEOUT ||
               XLinkSessionLifecycleGetState(session) >= XLINK_SESSION_STOPPING ||
               getXLinkState(link) != XLINK_UP)
                  ? X_LINK_COMMUNICATION_NOT_OPEN
                  : X_LINK_TIMEOUT;
    }
    return stopAndReapSession(handler, ret);
}

XLinkError_t XLinkResetRemote(XLinkHandler_t* handler)
{
    return XLinkResetRemoteTimeout(handler, XLINK_NO_RW_TIMEOUT);
}


XLinkError_t XLinkProfStart()
{
    XLINK_RET_IF(!XLinkGlobalHandlerIsValid());
    XLinkGlobalHandlerStartProfiling();
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkProfStop()
{
    XLINK_RET_IF(!XLinkGlobalHandlerIsValid());
    XLinkGlobalHandlerStopProfiling();
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkProfPrint()
{
    XLINK_RET_IF(!XLinkGlobalHandlerIsValid());
    XLinkGlobalHandlerPrintProfilingData();
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkGetGlobalProfilingData(XLinkProf_t* prof)
{
    XLINK_RET_IF(prof == NULL);
    XLINK_RET_IF(XLinkGlobalHandlerCopyProfilingData(prof));
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkGetProfilingData(XLinkHandler_t* handler, XLinkProf_t* prof)
{
    XLINK_RET_IF(prof == NULL);
    xLinkDesc_t* link = getLink(handler);
    XLINK_RET_IF(link == NULL);

    // TODO(themarpe) - thread safe readout
    *prof = link->profilingData;
    return X_LINK_SUCCESS;
}

// ------------------------------------
// API implementation. End.
// ------------------------------------


// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------

xLinkDesc_t* allocateSession(XLinkHandler_t* handler) {
    XLINK_RET_ERR_IF(handler == NULL, NULL);
    XLinkSession_t* session = calloc(1, sizeof(XLinkSession_t));
    XLINK_RET_ERR_IF(session == NULL, NULL);

    xLinkDesc_t* link = &session->link;
    session->linkDownCallback = handler->linkDownCallback;
    session->linkDownCallbackContext = handler->linkDownCallbackContext;
    if (XLinkSessionLifecycleInit(session) != 0) {
        free(session);
        return NULL;
    }

    if (XLink_sem_init(&link->pingSem, 0, 0)) {
        mvLog(MVLOG_ERROR, "Cannot initialize semaphore\n");
        XLinkSessionLifecycleDestroy(session);
        free(session);
        return NULL;
    }

    if (pthread_mutex_lock(&link_id_mutex) != 0) {
        XLink_sem_destroy(&link->pingSem);
        XLinkSessionLifecycleDestroy(session);
        free(session);
        return NULL;
    }
    link->id = nextLinkId;
    nextLinkId = (linkId_t)((nextLinkId + 1u) % INVALID_LINK_ID);
    if (pthread_mutex_unlock(&link_id_mutex) != 0) {
        XLink_sem_destroy(&link->pingSem);
        XLinkSessionLifecycleDestroy(session);
        free(session);
        return NULL;
    }

    link->peerState = XLINK_NOT_INIT;
    link->deviceHandle.session = session;
    for (int stream = 0; stream < XLINK_MAX_STREAMS; stream++) {
        link->availableStreams[stream].id = INVALID_STREAM_ID;
    }
    handler->session = session;
    handler->linkId = link->id;

    return link;
}

void deallocateSession(XLinkHandler_t* handler) {
    XLinkSession_t* session = getSession(handler);
    if (session == NULL) {
        return;
    }
    xLinkDesc_t* link = &session->link;

    if (XLink_sem_destroy(&link->pingSem)) {
        mvLog(MVLOG_ERROR, "Cannot destroy semaphore\n");
    }
    XLinkSessionLifecycleDestroy(session);

    if (handler != NULL) {
        handler->session = NULL;
        handler->linkId = INVALID_LINK_ID;
    }
    free(session);
}

static XLinkError_t stopAndReapSession(XLinkHandler_t* handler, XLinkError_t status) {
    xLinkDesc_t* link = getLink(handler);
    XLINK_RET_IF(link == NULL);
    XLinkSessionLifecycleBeginStop(getSession(handler), status == X_LINK_TIMEOUT ? X_LINK_LINK_DOWN_CONNECT_FAILURE : X_LINK_LINK_DOWN_LOCAL_RESET);
    xLinkSchedulerState_t* scheduler = getSchedulerFromDeviceHandle(&link->deviceHandle);
    XLINK_RET_IF(scheduler == NULL);

    if (DispatcherStop(&link->deviceHandle, 1)) {
        mvLog(MVLOG_ERROR, "failed to request dispatcher stop\n");
        return X_LINK_ERROR;
    }
    if (XLinkSessionLifecycleWaitStopped(getSession(handler)) != 0) {
        mvLog(MVLOG_ERROR, "failed waiting for stopped state\n");
        return X_LINK_ERROR;
    }
    if (pthread_join(scheduler->xLinkThreadId, NULL) != 0) {
        mvLog(MVLOG_ERROR, "can't join scheduler thread\n");
        return X_LINK_ERROR;
    }
    deallocateSession(handler);
    return status;
}

XLinkError_t parsePlatformError(xLinkPlatformErrorCode_t rc) {
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
        case X_LINK_PLATFORM_USB_EP_DRIVER_NOT_LOADED:
            return X_LINK_INIT_USB_ERROR;
        case X_LINK_PLATFORM_TCP_IP_DRIVER_NOT_LOADED:
            return X_LINK_INIT_TCP_IP_ERROR;
	case X_LINK_PLATFORM_LOCAL_SHDMEM_DRIVER_NOT_LOADED:
	    return X_LINK_INIT_LOCAL_SHDMEM_ERROR;
        case X_LINK_PLATFORM_PCIE_DRIVER_NOT_LOADED:
            return X_LINK_INIT_PCIE_ERROR;
	case X_LINK_PLATFORM_TCP_IP_OR_LOCAL_SHDMEM_DRIVER_NOT_LOADED:
        case X_LINK_PLATFORM_ERROR:
        case X_LINK_PLATFORM_INVALID_PARAMETERS:
        default:
            return X_LINK_ERROR;
    }
}

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
        case X_LINK_INIT_LOCAL_SHDMEM_ERROR: return "X_LINK_INIT_LOCAL_SHDMEM_ERROR";
        case X_LINK_INIT_TCP_IP_OR_LOCAL_SHDMEM_ERROR: return "X_LINK_INIT_TCP_IP_OR_LOCAL_SHDMEM_ERROR";
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
        case X_LINK_LOCAL_SHDMEM: return "X_LINK_LOCAL_SHDMEM";
        case X_LINK_TCP_IP_OR_LOCAL_SHDMEM: return "X_LINK_TCP_IP_OR_LOCAL_SHDMEM";
        case X_LINK_USB_EP: return "X_LINK_USB_EP";
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
        case X_LINK_RVC3: return "X_LINK_RVC3";
        case X_LINK_RVC4: return "X_LINK_RVC4";
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
        case X_LINK_GATE: return "X_LINK_GATE";
        case X_LINK_GATE_BOOTED: return "X_LINK_GATE_BOOTED";
        case X_LINK_GATE_SETUP: return "X_LINK_GATE_SETUP";
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
    }
    return "INVALID_ENUM_VALUE";
}

// ------------------------------------
// Helpers implementation. End.
// ------------------------------------
