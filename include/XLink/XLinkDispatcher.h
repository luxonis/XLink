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
 * @brief Detaches a local event from the waiting caller, when the caller
 *        stops waiting (for example after a timeout).
 *        If the dispatcher did not process the event yet or data for it
 *        came (EVENT_ALLOCATED, EVENT_READY), it waits for the result.
 * @param[in] deviceHandle - device handle of the event
 * @param[in] event - the event given to DispatcherAddEvent()
 * @return 1 if the event was still in the queue. The dispatcher does not
 *         write to "event" and does not post the semaphore of the caller.
 *         0 if the event was already served. "event" contains the result.
 */
int DispatcherCancelEvent(xLinkDeviceHandle_t *deviceHandle, xLinkEvent_t *event);
#ifdef __cplusplus
}
#endif

#endif
