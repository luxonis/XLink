// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///
/// @file
///
/// @brief     Application configuration Leon header
///
#ifndef _XLINKDISPATCHER_H
#define _XLINKDISPATCHER_H

#include "XLinkPrivateDefines.h"
#include "time.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C"
{
#endif
typedef int (*getRespFunction) (xLinkEvent_t*, xLinkEvent_t*, bool);
typedef struct {
    int (*eventSend) (xLinkEvent_t*, XLinkTimespec*);
    int (*eventReceive) (xLinkEvent_t*);
    getRespFunction localGetResponse;
    getRespFunction remoteGetResponse;
    void (*closeLink) (void* fd, int fullClose);
    void (*closeDeviceFd) (xLinkDeviceHandle_t* deviceHandle);
} DispatcherControlFunctions;

XLinkError_t DispatcherInitialize(DispatcherControlFunctions *controlFunc);
XLinkError_t DispatcherStart(xLinkDesc_t *deviceHandle);
XLinkError_t DispatcherStartServer(xLinkDesc_t *deviceHandle);
XLinkError_t DispatcherStartImpl(xLinkDesc_t *deviceHandle, bool server);
int DispatcherClean(xLinkDeviceHandle_t *deviceHandle);
int DispatcherDeviceFdDown(xLinkDeviceHandle_t *deviceHandle);

xLinkEvent_t* DispatcherAddEvent(xLinkEventOrigin_t origin, xLinkEvent_t *event);
xLinkEvent_t* DispatcherAddEvent_(xLinkEventOrigin_t origin, xLinkEvent_t *event, XLinkTimespec* outTime);
int DispatcherWaitEventComplete(xLinkDeviceHandle_t *deviceHandle, unsigned int timeoutMs);
int DispatcherWaitEventCompleteTimeout(xLinkDeviceHandle_t *deviceHandle, struct timespec abstime);

char* TypeToStr(int type);
int DispatcherUnblockEvent(eventId_t id,
                             xLinkEventType_t type,
                             streamId_t stream,
                             void *xlinkFD);
int DispatcherServeEvent(eventId_t id,
                             xLinkEventType_t type,
                             streamId_t stream,
                             void *xlinkFD);
/**
 * @brief Pipelining window of this side: the number of pipelined requests that
 *        this side accepts from the peer, and the most that it sends.
 *        XLINK_PIPELINE_WINDOW_DEFAULT, or the env var XLINK_PIPELINE_WINDOW
 *        (0 turns pipelining off), read in DispatcherInitialize().
 *        Not more than XLINK_PIPELINE_WINDOW_MAX.
 */
int DispatcherLocalPipelineWindow(void);
/**
 * @brief Sets the pipelining window of a link from the PING exchange.
 * @param[in] xLinkFD - link of the peer
 * @param[in] peerWindow - window announced by the peer, 0 for an old peer
 */
void DispatcherSetPeerPipelineWindow(void* xLinkFD, int peerWindow);
#ifdef __cplusplus
}
#endif

#endif
