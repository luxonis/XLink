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
    int (*eventSend) (xLinkEvent_t*);
    int (*eventReceive) (xLinkEvent_t*);
    getRespFunction localGetResponse;
    getRespFunction remoteGetResponse;
    void (*closeLink) (xLinkDeviceHandle_t deviceHandle);
    void (*closeDeviceFd) (xLinkDeviceHandle_t deviceHandle);
} DispatcherControlFunctions;

XLinkError_t DispatcherInitialize(DispatcherControlFunctions *controlFunc);
XLinkError_t DispatcherStart(xLinkDesc_t *deviceHandle);
XLinkError_t DispatcherStartServer(xLinkDesc_t *deviceHandle);
XLinkError_t DispatcherStartImpl(xLinkDesc_t *deviceHandle, bool server);
int DispatcherJoin(xLinkDeviceHandle_t *deviceHandle);
int DispatcherClean(xLinkDeviceHandle_t *deviceHandle);
int DispatcherDeviceFdDown(xLinkDeviceHandle_t *deviceHandle);

xLinkEvent_t* DispatcherAddEvent(xLinkEventOrigin_t origin, xLinkEvent_t *event);
int DispatcherWaitEventComplete(xLinkDeviceHandle_t deviceHandle, unsigned int timeoutMs);

char* TypeToStr(int type);
int DispatcherUnblockEvent(eventId_t id,
                            xLinkEventType_t type,
                            streamId_t stream,
                            void *xlinkFD);
int DispatcherServeOrDropEvent(eventId_t id,
                            xLinkEventType_t type,
                            streamId_t stream,
                            void *xlinkFD);
#ifdef __cplusplus
}
#endif

#endif