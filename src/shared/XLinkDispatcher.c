// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///
/// @file
///
/// @brief     Application configuration Leon header
///
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // fix for warning: implicit declaration of function 'pthread_setname_np'
#endif

#include <errno.h>
#include "stdio.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include <assert.h>
#include <stdlib.h>

#if (defined(_WIN32) || defined(_WIN64))
# include "win_pthread.h"
# include "win_semaphore.h"
#else
# include <pthread.h>
# include <unistd.h>
# ifndef __APPLE__
#  include <semaphore.h>
# endif
#endif

#include "XLinkDispatcher.h"
#include "XLinkMacros.h"
#include "XLinkPrivateDefines.h"
#include "XLinkPrivateFields.h"
#include "XLink.h"
#include "XLinkErrorUtils.h"

#define MVLOG_UNIT_NAME xLink
#include "XLinkLog.h"

// ------------------------------------
// Data structures declaration. Begin.
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
    XLink_sem_t* sem;
    void* data;
} xLinkEventPriv_t;

typedef struct {
    XLink_sem_t sem;
    pthread_t threadId;
} localSem_t;

typedef struct{
    xLinkEventPriv_t* end;
    xLinkEventPriv_t* base;

    xLinkEventPriv_t* curProc;
    xLinkEventPriv_t* cur;
    XLINK_ALIGN_TO_BOUNDARY(64) xLinkEventPriv_t q[MAX_EVENTS];

}eventQueueHandler_t;
/**
 * @brief Scheduler for each device
 */
typedef struct {
    xLinkDeviceHandle_t deviceHandle; //will be device handler
    int schedulerId;

    int queueProcPriority;

    pthread_mutex_t queueMutex;

    XLink_sem_t addEventSem;
    XLink_sem_t notifyDispatcherSem;
    volatile uint32_t resetXLink;
    uint32_t semaphores;
    pthread_t xLinkThreadId;

    eventQueueHandler_t lQueue; //local queue
    eventQueueHandler_t rQueue; //remote queue
    localSem_t eventSemaphores[MAXIMUM_SEMAPHORES];

    uint32_t dispatcherLinkDown;
    uint32_t dispatcherDeviceFdDown;
} xLinkSchedulerState_t;


// ------------------------------------
// Data structures declaration. Begin.
// ------------------------------------



// ------------------------------------
// Global fields declaration. Begin.
// ------------------------------------

//These will be common for all, Initialized only once
DispatcherControlFunctions* glControlFunc;
int numSchedulers;
xLinkSchedulerState_t schedulerState[MAX_SCHEDULERS];
sem_t addSchedulerSem;

static pthread_mutex_t unique_id_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clean_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reset_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t num_schedulers_mutex = PTHREAD_MUTEX_INITIALIZER;
// ------------------------------------
// Global fields declaration. End.
// ------------------------------------



// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

//below workaround for "C2088 '==': illegal for struct" error
static int pthread_t_compare(pthread_t a, pthread_t b);

static XLink_sem_t* createSem(xLinkSchedulerState_t* curr);
static XLink_sem_t* getSem(pthread_t threadId, xLinkSchedulerState_t *curr);

#if (defined(_WIN32) || defined(_WIN64))
static void* __cdecl eventReader(void* ctx);
static void* __cdecl eventSchedulerRun(void* ctx);
#else
static void* eventReader(void* ctx);
static void* eventSchedulerRun(void* ctx);
#endif

static int isEventTypeRequest(xLinkEventPriv_t* event);
static void postAndMarkEventServed(xLinkEventPriv_t *event);
static int createUniqueID();
static int findAvailableScheduler();
static xLinkSchedulerState_t* findCorrespondingScheduler(void* xLinkFD);

static int dispatcherRequestServe(xLinkEventPriv_t * event, xLinkSchedulerState_t* curr);
static int dispatcherResponseServe(xLinkEventPriv_t * event, xLinkSchedulerState_t* curr);

static inline xLinkEventPriv_t* getNextElementWithState(xLinkEventPriv_t* base, xLinkEventPriv_t* end,
                                                        xLinkEventPriv_t* start, xLinkEventState_t state);

static xLinkEventPriv_t* searchForReadyEvent(xLinkSchedulerState_t* curr);

static xLinkEventPriv_t* getNextQueueElemToProc(eventQueueHandler_t *q );
static xLinkEvent_t* addNextQueueElemToProc(xLinkSchedulerState_t* curr,
                                            eventQueueHandler_t *q, xLinkEvent_t* event,
                                            XLink_sem_t* sem, xLinkEventOrigin_t o);

static xLinkEventPriv_t* dispatcherGetNextEvent(xLinkSchedulerState_t* curr);

static int dispatcherClean(xLinkSchedulerState_t* curr);
static int dispatcherReset(xLinkSchedulerState_t* curr);
static int dispatcherDeviceFdDown(xLinkSchedulerState_t* curr);

static void dispatcherFreeEvents(eventQueueHandler_t *queue, xLinkEventState_t state);

static XLinkError_t sendEvents(xLinkSchedulerState_t* curr);

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkDispatcher.h implementation. Begin.
// ------------------------------------

XLinkError_t DispatcherInitialize(DispatcherControlFunctions *controlFunc) {
    ASSERT_XLINK(controlFunc != NULL);

    if (!controlFunc->eventReceive ||
        !controlFunc->eventSend ||
        !controlFunc->localGetResponse ||
        !controlFunc->remoteGetResponse) {
        return X_LINK_ERROR;
    }

    glControlFunc = controlFunc;
    numSchedulers = 0;

    if (sem_init(&addSchedulerSem, 0, 1)) {
        mvLog(MVLOG_ERROR, "Can't create semaphore\n");
        return X_LINK_ERROR;
    }

    for (int i = 0; i < MAX_SCHEDULERS; i++){
        schedulerState[i].schedulerId = -1;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t DispatcherStart(xLinkDeviceHandle_t *deviceHandle)
{
    ASSERT_XLINK(deviceHandle);
#ifdef __PC__
    ASSERT_XLINK(deviceHandle->xLinkFD != NULL);
#endif

    pthread_attr_t attr;
    int eventIdx;
    if (numSchedulers >= MAX_SCHEDULERS)
    {
        mvLog(MVLOG_ERROR,"Max number Schedulers reached!\n");
        return -1;
    }
    int idx = findAvailableScheduler();
    if (idx == -1) {
        mvLog(MVLOG_ERROR,"Max number Schedulers reached!\n");
        return -1;
    }

    memset(&schedulerState[idx], 0, sizeof(xLinkSchedulerState_t));

    schedulerState[idx].semaphores = 0;
    schedulerState[idx].queueProcPriority = 0;

    schedulerState[idx].resetXLink = 0;
    schedulerState[idx].dispatcherLinkDown = 0;
    schedulerState[idx].dispatcherDeviceFdDown = 0;

    schedulerState[idx].deviceHandle = *deviceHandle;
    schedulerState[idx].schedulerId = idx;

    schedulerState[idx].lQueue.cur = schedulerState[idx].lQueue.q;
    schedulerState[idx].lQueue.curProc = schedulerState[idx].lQueue.q;
    schedulerState[idx].lQueue.base = schedulerState[idx].lQueue.q;
    schedulerState[idx].lQueue.end = &schedulerState[idx].lQueue.q[MAX_EVENTS];

    schedulerState[idx].rQueue.cur = schedulerState[idx].rQueue.q;
    schedulerState[idx].rQueue.curProc = schedulerState[idx].rQueue.q;
    schedulerState[idx].rQueue.base = schedulerState[idx].rQueue.q;
    schedulerState[idx].rQueue.end = &schedulerState[idx].rQueue.q[MAX_EVENTS];

    for (eventIdx = 0 ; eventIdx < MAX_EVENTS; eventIdx++)
    {
        schedulerState[idx].rQueue.q[eventIdx].isServed = EVENT_SERVED;
        schedulerState[idx].lQueue.q[eventIdx].isServed = EVENT_SERVED;
    }

    if (XLink_sem_init(&schedulerState[idx].addEventSem, 0, 1)) {
        perror("Can't create semaphore\n");
        return -1;
    }
    if (pthread_mutex_init(&(schedulerState[idx].queueMutex), NULL) != 0) {
        perror("pthread_mutex_init error");
        return -1;
    }
    if (XLink_sem_init(&schedulerState[idx].notifyDispatcherSem, 0, 0)) {
        perror("Can't create semaphore\n");
    }
    localSem_t* temp = schedulerState[idx].eventSemaphores;
    while (temp < schedulerState[idx].eventSemaphores + MAXIMUM_SEMAPHORES) {
        XLink_sem_set_refs(&temp->sem, -1);
        temp++;
    }
    if (pthread_attr_init(&attr) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_init error");
        return X_LINK_ERROR;
    }

#ifndef __PC__
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_setinheritsched error");
        pthread_attr_destroy(&attr);
    }
    if (pthread_attr_setschedpolicy(&attr, SCHED_RR) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_setschedpolicy error");
        pthread_attr_destroy(&attr);
    }
#ifdef XLINK_THREADS_PRIORITY
    struct sched_param param;
    if (pthread_attr_getschedparam(&attr, &param) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_getschedparam error");
        pthread_attr_destroy(&attr);
    }
    param.sched_priority = XLINK_THREADS_PRIORITY;
    if (pthread_attr_setschedparam(&attr, &param) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_setschedparam error");
        pthread_attr_destroy(&attr);
    }
#endif
#endif

    while(((sem_wait(&addSchedulerSem) == -1) && errno == EINTR))
        continue;
    mvLog(MVLOG_DEBUG,"%s() starting a new thread - schedulerId %d \n", __func__, idx);
    int sc = pthread_create(&schedulerState[idx].xLinkThreadId,
                            &attr,
                            eventSchedulerRun,
                            (void*)&schedulerState[idx].schedulerId);
    if (sc) {
        mvLog(MVLOG_ERROR,"Thread creation failed with error: %d", sc);
        if (pthread_attr_destroy(&attr) != 0) {
            perror("Thread attr destroy failed\n");
        }
        return X_LINK_ERROR;
    }

#ifndef __APPLE__
    char schedulerThreadName[MVLOG_MAXIMUM_THREAD_NAME_SIZE];
    snprintf(schedulerThreadName, sizeof(schedulerThreadName), "Scheduler%.2dThr", schedulerState[idx].schedulerId);
    sc = pthread_setname_np(schedulerState[idx].xLinkThreadId, schedulerThreadName);
    if (sc != 0) {
        perror("Setting name for indexed scheduler thread failed");
    }
#endif

    pthread_detach(schedulerState[idx].xLinkThreadId);

    numSchedulers++;
    if (pthread_attr_destroy(&attr) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_destroy error");
    }

    sem_post(&addSchedulerSem);

    return 0;
}

int DispatcherClean(xLinkDeviceHandle_t *deviceHandle) {
    XLINK_RET_IF(deviceHandle == NULL);

    xLinkSchedulerState_t* curr = findCorrespondingScheduler(deviceHandle->xLinkFD);
    XLINK_RET_IF(curr == NULL);

    return dispatcherClean(curr);
}

int DispatcherDeviceFdDown(xLinkDeviceHandle_t *deviceHandle){
    XLINK_RET_IF(deviceHandle == NULL);

    xLinkSchedulerState_t* curr = findCorrespondingScheduler(deviceHandle->xLinkFD);
    XLINK_RET_IF(curr == NULL);

    return dispatcherDeviceFdDown(curr);
}

xLinkEvent_t* DispatcherAddEvent(xLinkEventOrigin_t origin, xLinkEvent_t *event)
{
    xLinkSchedulerState_t* curr = findCorrespondingScheduler(event->deviceHandle.xLinkFD);
    XLINK_RET_ERR_IF(curr == NULL, NULL);

    if(curr->resetXLink) {
        return NULL;
    }
    mvLog(MVLOG_DEBUG, "Receiving event %s %d\n", TypeToStr(event->header.type), origin);
    int rc;
    while(((rc = XLink_sem_wait(&curr->addEventSem)) == -1) && errno == EINTR)
        continue;
    if (rc) {
        mvLog(MVLOG_ERROR,"can't wait semaphore\n");
        return NULL;
    }

    XLink_sem_t *sem = NULL;
    xLinkEvent_t* ev;
    if (origin == EVENT_LOCAL) {
        event->header.id = createUniqueID();
        sem = getSem(pthread_self(), curr);
        if (!sem) {
            sem = createSem(curr);
        }
        if (!sem) {
            mvLog(MVLOG_WARN,"No more semaphores. Increase XLink or OS resources\n");
            if (XLink_sem_post(&curr->addEventSem)) {
                mvLog(MVLOG_ERROR,"can't post semaphore\n");
            }

            return NULL;
        }
        const uint32_t tmpMoveSem = event->header.flags.bitField.moveSemantic;
        event->header.flags.raw = 0;
        event->header.flags.bitField.moveSemantic = tmpMoveSem;
        ev = addNextQueueElemToProc(curr, &curr->lQueue, event, sem, origin);
    } else {
        ev = addNextQueueElemToProc(curr, &curr->rQueue, event, NULL, origin);
    }
    if (XLink_sem_post(&curr->addEventSem)) {
        mvLog(MVLOG_ERROR,"can't post semaphore\n");
    }
    if (XLink_sem_post(&curr->notifyDispatcherSem)) {
        mvLog(MVLOG_ERROR, "can't post semaphore\n");
    }
    return ev;
}

int DispatcherWaitEventComplete(xLinkDeviceHandle_t *deviceHandle, unsigned int timeoutMs)
{
    xLinkSchedulerState_t* curr = findCorrespondingScheduler(deviceHandle->xLinkFD);
    ASSERT_XLINK(curr != NULL);

    XLink_sem_t* id = getSem(pthread_self(), curr);
    if (id == NULL) {
        return -1;
    }

    int rc = 0;
    if (timeoutMs != XLINK_NO_RW_TIMEOUT) {
        // This is a workaround for sem_timedwait being influenced by the system clock change.
        // This is a temporary solution. TODO: replace this with something more efficient.
        while (timeoutMs--) {
            rc = XLink_sem_trywait(id);
            int tmpErrno = errno;
            if (rc == 0) {
                // Success
                break;
            } else {
                if(tmpErrno == ETIMEDOUT) {
#if (defined(_WIN32) || defined(_WIN64) )
                    Sleep(1);
#else
                    usleep(1000);
#endif
                } else {
                    // error, exit
                    break;
                }
            }
        }
    } else {
        while(((rc = XLink_sem_wait(id)) == -1) && errno == EINTR)
            continue;
    }
#ifdef __PC__
    if (rc) {
            xLinkEvent_t event = {0};
            event.header.type = XLINK_RESET_REQ;
            event.deviceHandle = *deviceHandle;
            mvLog(MVLOG_ERROR,"waiting is timeout, sending reset remote event");
            DispatcherAddEvent(EVENT_LOCAL, &event);
            id = getSem(pthread_self(), curr);
            int rc;
            while(((rc = XLink_sem_wait(id)) == -1) && errno == EINTR)
                continue;
            if (id == NULL || rc) {
                // // graceful link shutdown instead
                // dispatcherDeviceFdDown(curr);

                // Calling non-thread safe dispatcherReset from external thread
                // TODO - investigate further and resolve
                dispatcherReset(curr);
            }
        }
#endif

    return rc;
}

char* TypeToStr(int type)
{
    switch(type)
    {
        case XLINK_WRITE_REQ:     return "XLINK_WRITE_REQ";
        case XLINK_READ_REQ:      return "XLINK_READ_REQ";
        case XLINK_READ_REL_REQ:  return "XLINK_READ_REL_REQ";
        case XLINK_READ_REL_SPEC_REQ:  return "XLINK_READ_REL_SPEC_REQ";
        case XLINK_CREATE_STREAM_REQ:return "XLINK_CREATE_STREAM_REQ";
        case XLINK_CLOSE_STREAM_REQ: return "XLINK_CLOSE_STREAM_REQ";
        case XLINK_PING_REQ:         return "XLINK_PING_REQ";
        case XLINK_RESET_REQ:        return "XLINK_RESET_REQ";
        case XLINK_REQUEST_LAST:     return "XLINK_REQUEST_LAST";
        case XLINK_WRITE_RESP:   return "XLINK_WRITE_RESP";
        case XLINK_READ_RESP:     return "XLINK_READ_RESP";
        case XLINK_READ_REL_RESP: return "XLINK_READ_REL_RESP";
        case XLINK_READ_REL_SPEC_RESP:  return "XLINK_READ_REL_SPEC_RESP";
        case XLINK_CREATE_STREAM_RESP: return "XLINK_CREATE_STREAM_RESP";
        case XLINK_CLOSE_STREAM_RESP:  return "XLINK_CLOSE_STREAM_RESP";
        case XLINK_PING_RESP:  return "XLINK_PING_RESP";
        case XLINK_RESET_RESP: return "XLINK_RESET_RESP";
        case XLINK_RESP_LAST:  return "XLINK_RESP_LAST";
        default:
            break;
    }
    return "";
}

int DispatcherUnblockEvent(eventId_t id, xLinkEventType_t type, streamId_t stream, void *xlinkFD)
{
    xLinkSchedulerState_t* curr = findCorrespondingScheduler(xlinkFD);
    ASSERT_XLINK(curr != NULL);

    mvLog(MVLOG_DEBUG,"unblock\n");
    xLinkEventPriv_t* blockedEvent;

    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, 1);
    for (blockedEvent = curr->lQueue.q;
         blockedEvent < curr->lQueue.q + MAX_EVENTS;
         blockedEvent++)
    {
        if (blockedEvent->isServed == EVENT_BLOCKED &&
            ((blockedEvent->packet.header.id == id || id == -1)
             && blockedEvent->packet.header.type == type
             && blockedEvent->packet.header.streamId == stream))
        {
            mvLog(MVLOG_DEBUG,"unblocked**************** %d %s\n",
                  (int)blockedEvent->packet.header.id,
                  TypeToStr((int)blockedEvent->packet.header.type));
            blockedEvent->isServed = EVENT_READY;
            if (XLink_sem_post(&curr->notifyDispatcherSem)){
                mvLog(MVLOG_ERROR, "can't post semaphore\n");
            }
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);
            return 1;
        } else {
            mvLog(MVLOG_DEBUG,"%d %s\n",
                  (int)blockedEvent->packet.header.id,
                  TypeToStr((int)blockedEvent->packet.header.type));
        }
    }
    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);
    return 0;
}

int DispatcherServeEvent(eventId_t id, xLinkEventType_t type, streamId_t stream, void *xlinkFD)
{
    xLinkSchedulerState_t* curr = findCorrespondingScheduler(xlinkFD);
    ASSERT_XLINK(curr != NULL);

    xLinkEventPriv_t* event;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, 1);
    for (event = curr->lQueue.q;
         event < curr->lQueue.q + MAX_EVENTS;
         event++)
    {
        if (((event->packet.header.id == id || id == -1)
             && event->packet.header.type == type
             && event->packet.header.streamId == stream))
        {
            mvLog(MVLOG_DEBUG,"served**************** %d %s\n",
                  (int)event->packet.header.id,
                  TypeToStr((int)event->packet.header.type));
            event->isServed = EVENT_SERVED;
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);
            return 1;
        }
    }
    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);
    return 0;
}

// ------------------------------------
// XLinkDispatcher.h implementation. End.
// ------------------------------------



// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------

int pthread_t_compare(pthread_t a, pthread_t b)
{
#if (defined(_WIN32) || defined(_WIN64) )
#ifdef __GNUC__
    return  (a == b);
#else
    return ((a.tid == b.tid));
#endif
#else
    return  (a == b);
#endif
}

static XLink_sem_t* createSem(xLinkSchedulerState_t* curr)
{
    XLINK_RET_ERR_IF(curr == NULL, NULL);

    XLink_sem_t* sem = getSem(pthread_self(), curr);
    if (sem) {// it already exists, error
        return NULL;
    }

    if (curr->semaphores <= MAXIMUM_SEMAPHORES) {
        localSem_t* temp = curr->eventSemaphores;

        while (temp < curr->eventSemaphores + MAXIMUM_SEMAPHORES) {
            int refs = 0;
            XLINK_RET_ERR_IF(XLink_sem_get_refs(&temp->sem, &refs), NULL);
            if (refs < 0 || curr->semaphores == MAXIMUM_SEMAPHORES) {
                if (curr->semaphores == MAXIMUM_SEMAPHORES && refs == 0) {
                    XLINK_RET_ERR_IF(XLink_sem_destroy(&temp->sem), NULL);
                    XLINK_RET_ERR_IF(XLink_sem_get_refs(&temp->sem, &refs), NULL);
                    curr->semaphores --;
#if (defined(_WIN32) || defined(_WIN64))
                    memset(&temp->threadId, 0, sizeof(temp->threadId));
#else
                    temp->threadId = 0;
#endif
                }

                if (refs == -1) {
                    sem = &temp->sem;
                    if (XLink_sem_init(sem, 0, 0)){
                        mvLog(MVLOG_ERROR, "Error: Can't create semaphore\n");
                        return NULL;
                    }
                    curr->semaphores++;
                    temp->threadId = pthread_self();
                    break;
                }
            }
            temp++;
        }
        if (!sem) {
            return NULL; //shouldn't happen
        }
    }
    else {
        mvLog(MVLOG_ERROR, "Error: cached semaphores %d exceeds the MAXIMUM_SEMAPHORES %d", curr->semaphores, MAXIMUM_SEMAPHORES);
        return NULL;
    }

    return sem;
}

static XLink_sem_t* getSem(pthread_t threadId, xLinkSchedulerState_t *curr)
{
    XLINK_RET_ERR_IF(curr == NULL, NULL);

    localSem_t* temp = curr->eventSemaphores;
    while (temp < curr->eventSemaphores + MAXIMUM_SEMAPHORES) {
        int refs = 0;
        XLINK_RET_ERR_IF(XLink_sem_get_refs(&temp->sem, &refs), NULL);
        if (pthread_t_compare(temp->threadId, threadId) && refs >= 0) {
            return &temp->sem;
        }
        temp++;
    }
    return NULL;
}

#if (defined(_WIN32) || defined(_WIN64))
static void* __cdecl eventReader(void* ctx)
#else
static void* eventReader(void* ctx)
#endif
{
    xLinkSchedulerState_t *curr = (xLinkSchedulerState_t*)ctx;
    XLINK_RET_ERR_IF(curr == NULL, NULL);

    xLinkEvent_t event = { 0 };// to fix error C4700 in win
    event.header.id = -1;
    event.deviceHandle = curr->deviceHandle;

    mvLog(MVLOG_INFO,"eventReader thread started");

    while (!curr->resetXLink) {
        int sc = glControlFunc->eventReceive(&event);

        mvLog(MVLOG_DEBUG,"Reading %s (scheduler %d, fd %p, event id %d, event stream_id %u, event size %u)\n",
              TypeToStr(event.header.type), curr->schedulerId, event.deviceHandle.xLinkFD, event.header.id, event.header.streamId, event.header.size);

        if (sc) {
            mvLog(MVLOG_DEBUG,"Failed to receive event (err %d)", sc);
            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, NULL);
            dispatcherFreeEvents(&curr->lQueue, EVENT_PENDING);
            dispatcherFreeEvents(&curr->lQueue, EVENT_BLOCKED);
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
            continue;
        }

        DispatcherAddEvent(EVENT_REMOTE, &event);

        if (event.header.type == XLINK_RESET_REQ) {
            curr->resetXLink = 1;
            mvLog(MVLOG_DEBUG,"Read XLINK_RESET_REQ, stopping eventReader thread.");
        }
    }

    return 0;
}

#if (defined(_WIN32) || defined(_WIN64))
static void* __cdecl eventSchedulerRun(void* ctx)
#else
static void* eventSchedulerRun(void* ctx)
#endif
{
    int schedulerId = *((int*) ctx);
    mvLog(MVLOG_DEBUG,"%s() schedulerId %d\n", __func__, schedulerId);
    XLINK_RET_ERR_IF(schedulerId >= MAX_SCHEDULERS, NULL);

    xLinkSchedulerState_t* curr = &schedulerState[schedulerId];
    pthread_t readerThreadId;        /* Create thread for reader.
                        This thread will notify the dispatcher of any incoming packets*/
    pthread_attr_t attr;
    int sc;
    if (pthread_attr_init(&attr) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_init error");
        return NULL;
    }

#ifndef __PC__
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
        pthread_attr_destroy(&attr);
        mvLog(MVLOG_ERROR,"pthread_attr_setinheritsched error");
        return NULL;
    }
    if (pthread_attr_setschedpolicy(&attr, SCHED_RR) != 0) {
        pthread_attr_destroy(&attr);
        mvLog(MVLOG_ERROR,"pthread_attr_setschedpolicy error");
        return NULL;
    }
#ifdef XLINK_THREADS_PRIORITY
    struct sched_param param;
    if (pthread_attr_getschedparam(&attr, &param) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_getschedparam error");
        pthread_attr_destroy(&attr);
    }
    param.sched_priority = XLINK_THREADS_PRIORITY;
    if (pthread_attr_setschedparam(&attr, &param) != 0) {
        mvLog(MVLOG_ERROR,"pthread_attr_setschedparam error");
        pthread_attr_destroy(&attr);
    }
#endif
#endif
    sc = pthread_create(&readerThreadId, &attr, eventReader, curr);
    if (sc) {
        mvLog(MVLOG_ERROR, "Thread creation failed");
        if (pthread_attr_destroy(&attr) != 0) {
            perror("Thread attr destroy failed\n");
        }
        return NULL;
    }
#ifndef __APPLE__
    char eventReaderThreadName[MVLOG_MAXIMUM_THREAD_NAME_SIZE];
    snprintf(eventReaderThreadName, sizeof(eventReaderThreadName), "EventRead%.2dThr", schedulerId);
    sc = pthread_setname_np(readerThreadId, eventReaderThreadName);
    if (sc != 0) {
        perror("Setting name for event reader thread failed");
    }
#endif
    mvLog(MVLOG_INFO,"Scheduler thread started");

    XLinkError_t rc = sendEvents(curr);
    if(rc) {
        mvLog(MVLOG_ERROR, "sendEvents method finished with an error: %s", XLinkErrorToStr(rc));
    }

    sc = pthread_join(readerThreadId, NULL);
    if (sc) {
        mvLog(MVLOG_ERROR, "Waiting for thread failed");
    }

    sc = pthread_attr_destroy(&attr);
    if (sc) {
        mvLog(MVLOG_WARN, "Thread attr destroy failed");
    }

    if (dispatcherReset(curr) != 0) {
        mvLog(MVLOG_WARN, "Failed to reset or was already reset");
    }

    if (curr->resetXLink != 1) {
        mvLog(MVLOG_ERROR,"Scheduler thread stopped");
    } else {
        mvLog(MVLOG_INFO,"Scheduler thread stopped");
    }

    return NULL;
}

static int isEventTypeRequest(xLinkEventPriv_t* event)
{
    return event->packet.header.type < XLINK_REQUEST_LAST;
}

static void postAndMarkEventServed(xLinkEventPriv_t *event)
{
    if (event->retEv){
        // the xLinkEventPriv_t slot pointed by "event" will be
        // re-cycled as soon as we mark it as EVENT_SERVED,
        // so before that, we copy the result event into XLink API layer
        *(event->retEv) = event->packet;
    }
    if(event->sem){
        if (XLink_sem_post(event->sem)) {
            mvLog(MVLOG_ERROR,"can't post semaphore\n");
        }
    }

    event->isServed = EVENT_SERVED;
}

static int createUniqueID()
{
    static eventId_t id = 0xa;
    eventId_t idCopy = 0;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&unique_id_mutex) != 0, -1);
    id++;
    if(id >= INT32_MAX){
        id = 0xa;
    }
    idCopy = id;
    XLINK_RET_ERR_IF(pthread_mutex_unlock(&unique_id_mutex) != 0, -1);

    return idCopy;
}

int findAvailableScheduler()
{
    int i;
    for (i = 0; i < MAX_SCHEDULERS; i++)
        if (schedulerState[i].schedulerId == -1)
            return i;
    return -1;
}

static xLinkSchedulerState_t* findCorrespondingScheduler(void* xLinkFD)
{
    int i;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&num_schedulers_mutex) != 0, NULL);
    if (xLinkFD == NULL) { //in case of myriad there should be one scheduler
        if (numSchedulers == 1) {
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&num_schedulers_mutex) != 0, NULL);
            return &schedulerState[0];
        } else {
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&num_schedulers_mutex) != 0, NULL);
            return NULL;
        }
    }
    for (i=0; i < MAX_SCHEDULERS; i++)
        if (schedulerState[i].schedulerId != -1 &&
            schedulerState[i].deviceHandle.xLinkFD == xLinkFD) {
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&num_schedulers_mutex) != 0, NULL);
                return &schedulerState[i];
        }

    XLINK_RET_ERR_IF(pthread_mutex_unlock(&num_schedulers_mutex) != 0, NULL);
    return NULL;
}

static int dispatcherRequestServe(xLinkEventPriv_t * event, xLinkSchedulerState_t* curr){
    XLINK_RET_IF(curr == NULL);
    XLINK_RET_IF(!isEventTypeRequest(event));
    xLinkEventHeader_t *header = &event->packet.header;
    if (header->flags.bitField.block){ //block is requested
        event->isServed = EVENT_BLOCKED;
    } else if(header->flags.bitField.localServe == 1 ||
              (header->flags.bitField.ack == 0
               && header->flags.bitField.nack == 1)){ //this event is served locally, or it is failed
        postAndMarkEventServed(event);
    } else if (header->flags.bitField.ack == 1
              && header->flags.bitField.nack == 0){
        event->isServed = EVENT_PENDING;
        mvLog(MVLOG_DEBUG,"------------------------UNserved %s\n",
              TypeToStr(event->packet.header.type));
    }else{
        return 1;
    }
    return 0;
}

static int dispatcherResponseServe(xLinkEventPriv_t * event, xLinkSchedulerState_t* curr)
{
    XLINK_RET_ERR_IF(curr == NULL, 1);
    XLINK_RET_ERR_IF(isEventTypeRequest(event), 1);
    int i = 0;
    for (i = 0; i < MAX_EVENTS; i++)
    {
        xLinkEventHeader_t *header = &curr->lQueue.q[i].packet.header;
        xLinkEventHeader_t *evHeader = &event->packet.header;

        if (curr->lQueue.q[i].isServed == EVENT_PENDING &&
            header->id == evHeader->id &&
            header->type == evHeader->type - XLINK_REQUEST_LAST -1)
        {
            mvLog(MVLOG_DEBUG,"----------------------ISserved %s\n",
                  TypeToStr(header->type));
            //propagate back flags
            header->flags = evHeader->flags;
            postAndMarkEventServed(&curr->lQueue.q[i]);
            break;
        }
    }
    if (i == MAX_EVENTS) {
        mvLog(MVLOG_FATAL,"no request for this response: %s %d\n", TypeToStr(event->packet.header.type), event->origin);
        mvLog(MVLOG_DEBUG,"#### (i == MAX_EVENTS) %s %d %d\n", TypeToStr(event->packet.header.type), event->origin, (int)event->packet.header.id);
        for (i = 0; i < MAX_EVENTS; i++)
        {
            xLinkEventHeader_t *header = &curr->lQueue.q[i].packet.header;

            mvLog(MVLOG_DEBUG,"%d) header->id %i, header->type %s(%i), curr->lQueue.q[i].isServed %i, EVENT_PENDING %i\n", i, (int)header->id
            , TypeToStr(header->type), header->type, curr->lQueue.q[i].isServed, EVENT_PENDING);

        }
        return 1;
    }
    return 0;
}

static inline xLinkEventPriv_t* getNextElementWithState(xLinkEventPriv_t* base, xLinkEventPriv_t* end,
                                                        xLinkEventPriv_t* start, xLinkEventState_t state){
    xLinkEventPriv_t* tmp = start;
    while (start->isServed != state){
        CIRCULAR_INCREMENT_BASE(start, end, base);
        if(tmp == start){
            break;
        }
    }
    if(start->isServed == state){
        return start;
    }else{
        return NULL;
    }
}

static xLinkEventPriv_t* searchForReadyEvent(xLinkSchedulerState_t* curr)
{
    XLINK_RET_ERR_IF(curr == NULL, NULL);
    xLinkEventPriv_t* ev = NULL;

    ev = getNextElementWithState(curr->lQueue.base, curr->lQueue.end, curr->lQueue.base, EVENT_READY);
    if(ev){
        mvLog(MVLOG_DEBUG,"ready %s %d \n",
              TypeToStr((int)ev->packet.header.type),
              (int)ev->packet.header.id);
    }
    return ev;
}

static xLinkEventPriv_t* getNextQueueElemToProc(eventQueueHandler_t *q ){
    xLinkEventPriv_t* event = NULL;
    if (q->cur != q->curProc) {
        event = getNextElementWithState(q->base, q->end, q->curProc, EVENT_ALLOCATED);
        q->curProc = event;
        CIRCULAR_INCREMENT_BASE(q->curProc, q->end, q->base);
    }
    return event;
}

/**
 * @brief Add event to Queue
 * @note It called from dispatcherAddEvent
 */
static xLinkEvent_t* addNextQueueElemToProc(xLinkSchedulerState_t* curr,
                                            eventQueueHandler_t *q, xLinkEvent_t* event,
                                            XLink_sem_t* sem, xLinkEventOrigin_t o){
    xLinkEvent_t* ev;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, NULL);
    xLinkEventPriv_t* eventP = getNextElementWithState(q->base, q->end, q->cur, EVENT_SERVED);
    if (eventP == NULL) {
        mvLog(MVLOG_ERROR, "getNextElementWithState returned NULL");
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
        return NULL;
    }
    mvLog(MVLOG_DEBUG, "Received event %s %d", TypeToStr(event->header.type), o);
    ev = &eventP->packet;

    eventP->sem = sem;
    eventP->packet = *event;
    eventP->origin = o;
    if (o == EVENT_LOCAL) {
        // XLink API caller provided buffer for return the final result to
        eventP->retEv = event;
    }else{
        eventP->retEv = NULL;
    }
    q->cur = eventP;
    eventP->isServed = EVENT_ALLOCATED;
    CIRCULAR_INCREMENT_BASE(q->cur, q->end, q->base);
    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
    return ev;
}

static xLinkEventPriv_t* dispatcherGetNextEvent(xLinkSchedulerState_t* curr)
{
    XLINK_RET_ERR_IF(curr == NULL, NULL);

    int rc;
    while(((rc = XLink_sem_wait(&curr->notifyDispatcherSem)) == -1) && errno == EINTR)
        continue;
    if (rc) {
        mvLog(MVLOG_ERROR,"can't post semaphore\n");
    }

    xLinkEventPriv_t* event = NULL;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, NULL);
    event = searchForReadyEvent(curr);
    if (event) {
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
        return event;
    }

    eventQueueHandler_t* hPriorityQueue = curr->queueProcPriority ? &curr->lQueue : &curr->rQueue;
    eventQueueHandler_t* lPriorityQueue = curr->queueProcPriority ? &curr->rQueue : &curr->lQueue;
    curr->queueProcPriority = curr->queueProcPriority ? 0 : 1;

    event = getNextQueueElemToProc(hPriorityQueue);
    if (event) {
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
        return event;
    }
    event = getNextQueueElemToProc(lPriorityQueue);

    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
    return event;
}

static int dispatcherClean(xLinkSchedulerState_t* curr)
{
    XLINK_RET_ERR_IF(pthread_mutex_lock(&clean_mutex), 1);
    if (curr->schedulerId == -1) {
        mvLog(MVLOG_WARN,"Scheduler has already been reset or cleaned");
        if(pthread_mutex_unlock(&clean_mutex) != 0) {
            mvLog(MVLOG_ERROR, "Failed to unlock clean_mutex");
        }

        return 1;
    }

    mvLog(MVLOG_INFO, "Start Clean Dispatcher...");

    if (XLink_sem_post(&curr->notifyDispatcherSem)) {
        mvLog(MVLOG_ERROR,"can't post semaphore\n"); //to allow us to get a NULL event
    }
    xLinkEventPriv_t* event = dispatcherGetNextEvent(curr);
    while (event != NULL) {
        mvLog(MVLOG_INFO, "dropped event is %s, status %d\n",
              TypeToStr(event->packet.header.type), event->isServed);

        XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, 1);
        postAndMarkEventServed(event);
        XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);
        event = dispatcherGetNextEvent(curr);
    }

    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, 1);

    dispatcherFreeEvents(&curr->lQueue, EVENT_PENDING);
    dispatcherFreeEvents(&curr->lQueue, EVENT_BLOCKED);

    curr->schedulerId = -1;
    curr->resetXLink = 1;
    XLink_sem_destroy(&curr->addEventSem);
    XLink_sem_destroy(&curr->notifyDispatcherSem);
    localSem_t* temp = curr->eventSemaphores;
    while (temp < curr->eventSemaphores + MAXIMUM_SEMAPHORES) {
        // unblock potentially blocked event semaphores
        XLink_sem_post(&temp->sem);
        XLink_sem_destroy(&temp->sem);
        temp++;
    }
    numSchedulers--;

    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, 1);

    mvLog(MVLOG_INFO, "Clean Dispatcher Successfully...");
    if(pthread_mutex_unlock(&clean_mutex) != 0) {
        mvLog(MVLOG_ERROR, "Failed to unlock clean_mutex after clearing dispatcher");
    }
    XLINK_RET_ERR_IF(pthread_mutex_destroy(&(curr->queueMutex)) != 0, 1);
    return 0;
}

static int dispatcherDeviceFdDown(xLinkSchedulerState_t* curr){
    ASSERT_XLINK(curr != NULL);
    XLINK_RET_ERR_IF(pthread_mutex_lock(&reset_mutex), 1);
    int ret = 0;

    if (curr->dispatcherDeviceFdDown == 0) {

        glControlFunc->closeDeviceFd(&curr->deviceHandle);
        // Specify device FD was already closed
        curr->dispatcherDeviceFdDown = 1;

    } else {
        ret = 1;
    }

    if(pthread_mutex_unlock(&reset_mutex) != 0) {
        mvLog(MVLOG_ERROR, "Failed to unlock reset_mutex");
        ret = 1;
    }

    return ret;
}

static int dispatcherReset(xLinkSchedulerState_t* curr)
{
    ASSERT_XLINK(curr != NULL);

    XLINK_RET_ERR_IF(pthread_mutex_lock(&reset_mutex), 1);
    if (curr->dispatcherLinkDown == 1) {
        mvLog(MVLOG_WARN,"Scheduler has already been reset");
        if(pthread_mutex_unlock(&reset_mutex) != 0) {
            mvLog(MVLOG_ERROR, "Failed to unlock clean_mutex");
        }

        return 1;
    }

    if(!curr->dispatcherDeviceFdDown){
        glControlFunc->closeDeviceFd(&curr->deviceHandle);
        // Specify device FD was already closed
        curr->dispatcherDeviceFdDown = 1;
    }

    if(dispatcherClean(curr)) {
        mvLog(MVLOG_INFO, "Failed to clean dispatcher");
    }

    xLinkDesc_t* link = getLink(curr->deviceHandle.xLinkFD);
    if(link == NULL || XLink_sem_post(&link->dispatcherClosedSem)) {
        mvLog(MVLOG_DEBUG,"can't post dispatcherClosedSem\n");
    }

    glControlFunc->closeLink(curr->deviceHandle.xLinkFD, 1);

    // Set dispatcher link state "down", to disallow resetting again
    curr->dispatcherLinkDown = 1;
    mvLog(MVLOG_DEBUG,"Reset Successfully\n");

    if(pthread_mutex_unlock(&reset_mutex) != 0) {
        mvLog(MVLOG_ERROR, "Failed to unlock clean_mutex after clearing dispatcher");
        return 1;
    }

    return 0;
}

static XLinkError_t sendEvents(xLinkSchedulerState_t* curr) {
    int res;
    xLinkEventPriv_t* event;
    xLinkEventPriv_t response;

    while (!curr->resetXLink) {
        event = dispatcherGetNextEvent(curr);
        if(event == NULL) {
            mvLog(MVLOG_ERROR,"Dispatcher received NULL event!");
#ifdef __PC__
            break; //Mean that user reset XLink.
#else
            continue;
#endif
        }

        if(event->packet.deviceHandle.xLinkFD
           != curr->deviceHandle.xLinkFD) {
            mvLog(MVLOG_FATAL,"The file descriptor mismatch between the event and the scheduler.\n"
                              "    Event: id=%d, fd=%p"
                              "    Scheduler fd=%p",
                              event->packet.header.id, event->packet.deviceHandle.xLinkFD,
                              curr->deviceHandle.xLinkFD);
            event->packet.header.flags.bitField.nack = 1;
            event->packet.header.flags.bitField.ack = 0;

            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
            if (event->origin == EVENT_LOCAL){
                dispatcherRequestServe(event, curr);
            } else {
                dispatcherResponseServe(event, curr);
            }
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, X_LINK_ERROR);

            continue;
        }

        getRespFunction getResp;
        xLinkEvent_t* toSend;
        if (event->origin == EVENT_LOCAL){
            getResp = glControlFunc->localGetResponse;
            toSend = &event->packet;
        }else{
            getResp = glControlFunc->remoteGetResponse;
            toSend = &response.packet;
        }

        res = getResp(&event->packet, &response.packet);
        if (isEventTypeRequest(event)) {
            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
            if (event->origin == EVENT_LOCAL) { //we need to do this for locals only
                if(dispatcherRequestServe(event, curr)) {
                    mvLog(MVLOG_ERROR, "Failed to serve local event. "
                                       "Event: id=%d, type=%s, streamId=%u, streamName=%s",
                                       event->packet.header.id,  TypeToStr(event->packet.header.type),
                                       event->packet.header.streamId, event->packet.header.streamName);
                }
            }

            if (res == 0 && event->packet.header.flags.bitField.localServe == 0) {
#ifdef __PC__
                if (toSend->header.type == XLINK_RESET_REQ) {
                    curr->resetXLink = 1;
                    mvLog(MVLOG_DEBUG,"Send XLINK_RESET_REQ, stopping sendEvents thread.");
                    if(toSend->deviceHandle.protocol == X_LINK_PCIE) {
                        toSend->header.type = XLINK_PING_REQ;
                        mvLog(MVLOG_DEBUG, "Request for reboot not sent, only ping event");
                    } else {
#if defined(NO_BOOT)
                        toSend->header.type = XLINK_PING_REQ;
                        mvLog(MVLOG_INFO, "Request for reboot not sent, only ping event");
#endif // defined(NO_BOOT)

                    }
                }
#endif // __PC__
                XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
                if (glControlFunc->eventSend(toSend) != 0) {
                    XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
                    dispatcherFreeEvents(&curr->lQueue, EVENT_PENDING);
                    dispatcherFreeEvents(&curr->lQueue, EVENT_BLOCKED);
                    XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
                    mvLog(MVLOG_ERROR, "Event sending failed");
                }
            } else {
                XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
            }
        } else {
            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
            if (event->origin == EVENT_REMOTE){ // match remote response with the local request
                dispatcherResponseServe(event, curr);
            }
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, X_LINK_ERROR);
        }

        if (event->origin == EVENT_REMOTE){
            event->isServed = EVENT_SERVED;
        }
    }

    return X_LINK_SUCCESS;
}

static void dispatcherFreeEvents(eventQueueHandler_t *queue, xLinkEventState_t state) {
    if(queue == NULL) {
        return;
    }

    xLinkEventPriv_t* event = getNextElementWithState(queue->base, queue->end, queue->base, state);
    while (event != NULL) {
        mvLog(MVLOG_DEBUG, "Event is %s, size is %d, Mark it served\n", TypeToStr(event->packet.header.type), event->packet.header.size);
        postAndMarkEventServed(event);
        event = getNextElementWithState(queue->base, queue->end, queue->base, state);
    }
}


// ------------------------------------
// Helpers implementation. End.
// ------------------------------------
