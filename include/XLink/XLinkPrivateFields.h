// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifndef _XLINKPRIVATEFIELDS_H
#define _XLINKPRIVATEFIELDS_H

#include "XLinkDispatcher.h"

#define LINK_ID_MASK 0xFF
#define LINK_ID_SHIFT ((sizeof(uint32_t) - sizeof(uint8_t)) * 8)
#define STREAM_ID_MASK 0xFFFFFF

#define EXTRACT_LINK_ID(streamId) (((streamId) >> LINK_ID_SHIFT) & LINK_ID_MASK)
#define EXTRACT_STREAM_ID(streamId) ((streamId) & STREAM_ID_MASK)

#define COMBINE_IDS(streamId, linkid) \
    streamId = streamId | (((uint32_t)linkid & LINK_ID_MASK) << LINK_ID_SHIFT);

// ------------------------------------
// Global fields declaration. Begin.
// ------------------------------------

extern DispatcherControlFunctions controlFunctionTbl;
extern sem_t  pingSem; //to b used by myriad

// ------------------------------------
// Global fields declaration. End.
// ------------------------------------

typedef enum {
    EVENT_ALLOCATED,
    EVENT_PENDING,
    EVENT_BLOCKED,
    EVENT_READY,
    EVENT_SERVED,
} xLinkEventState_t;

typedef struct xLinkEventPriv_t {
    xLinkEvent_t packet;
    xLinkEvent_t *retEv;
    xLinkEventState_t isServed;
    xLinkEventOrigin_t origin;
    XLinkTimespec* sendTime;
    XLink_sem_t* sem;
    void* data;
} xLinkEventPriv_t;

typedef struct {
    XLink_sem_t sem;
    pthread_t threadId;
} localSem_t;

typedef struct {
    xLinkEventPriv_t* end;
    xLinkEventPriv_t* base;
    xLinkEventPriv_t* curProc;
    xLinkEventPriv_t* cur;
    XLINK_ALIGN_TO_BOUNDARY(64) xLinkEventPriv_t q[MAX_EVENTS];
} eventQueueHandler_t;

typedef struct xLinkSchedulerState_t {
    xLinkDeviceHandle_t deviceHandle;
    xLinkDesc_t* link;
    int schedulerId;
    int queueProcPriority;
    pthread_mutex_t queueMutex;
    XLink_sem_t addEventSem;
    XLink_sem_t notifyDispatcherSem;
    volatile uint32_t resetXLink;
    uint32_t semaphores;
    pthread_t xLinkThreadId;
    eventQueueHandler_t lQueue;
    eventQueueHandler_t rQueue;
    localSem_t eventSemaphores[MAXIMUM_SEMAPHORES];
    uint32_t dispatcherLinkDown;
    uint32_t dispatcherDeviceFdDown;
    uint32_t server;
} xLinkSchedulerState_t;

struct XLinkSession_t {
    XLinkLinkDownCallback_t linkDownCallback;
    void* linkDownCallbackContext;
    xLinkDesc_t link;
    xLinkSchedulerState_t scheduler;
};

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

xLinkDesc_t* getLink(const XLinkHandler_t* handler);
XLinkSession_t* getSession(const XLinkHandler_t* handler);
xLinkDesc_t* getLinkFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle);
XLinkSession_t* getSessionFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle);
xLinkSchedulerState_t* getSchedulerFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle);
xLinkState_t getXLinkState(xLinkDesc_t* link);


streamId_t getStreamIdByName(xLinkDesc_t* link, const char* name);

streamDesc_t* getStreamById(xLinkDesc_t* link, streamId_t id);
streamDesc_t* getStreamByName(xLinkDesc_t* link, const char* name);

void releaseStream(streamDesc_t* stream);

#ifdef __cplusplus
extern "C" {
#endif
void XLinkGlobalHandlerAssign(XLinkGlobalHandler_t* globalHandler);
int XLinkGlobalHandlerIsValid(void);
void XLinkGlobalHandlerStartProfiling(void);
void XLinkGlobalHandlerStopProfiling(void);
void XLinkGlobalHandlerAccumulateRead(uint32_t bytes, float timeSeconds);
void XLinkGlobalHandlerAccumulateWrite(uint32_t bytes, float timeSeconds);
int XLinkGlobalHandlerCopyProfilingData(XLinkProf_t* prof);
void XLinkGlobalHandlerPrintProfilingData(void);
#ifdef __cplusplus
}
#endif

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------

#endif //PROJECT_XLINKPRIVATEFIELDS_H
