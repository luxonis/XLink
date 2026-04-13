// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "stdlib.h"

#include "XLinkPrivateFields.h"
#include "XLinkPrivateDefines.h"
#include "XLinkErrorUtils.h"

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif

#include "XLinkLog.h"

XLinkSession_t* getSession(const XLinkHandler_t* handler)
{
    XLINK_RET_ERR_IF(handler == NULL, NULL);
    return handler->session;
}

xLinkDesc_t* getLink(const XLinkHandler_t* handler)
{
    XLinkSession_t* session = getSession(handler);
    XLINK_RET_ERR_IF(session == NULL, NULL);
    return &session->link;
}

XLinkSession_t* getSessionFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle)
{
    XLINK_RET_ERR_IF(deviceHandle == NULL, NULL);
    return deviceHandle->session;
}

xLinkDesc_t* getLinkFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle)
{
    XLinkSession_t* session = getSessionFromDeviceHandle(deviceHandle);
    XLINK_RET_ERR_IF(session == NULL, NULL);
    return &session->link;
}

xLinkSchedulerState_t* getSchedulerFromDeviceHandle(const xLinkDeviceHandle_t* deviceHandle)
{
    XLinkSession_t* session = getSessionFromDeviceHandle(deviceHandle);
    XLINK_RET_ERR_IF(session == NULL, NULL);
    return &session->scheduler;
}

streamId_t getStreamIdByName(xLinkDesc_t* link, const char* name)
{
    streamDesc_t* stream = getStreamByName(link, name);

    if (stream) {
        streamId_t id = stream->id;
        releaseStream(stream);
        return id;
    }

    return INVALID_STREAM_ID;
}

streamDesc_t* getStreamById(xLinkDesc_t* link, streamId_t id)
{
    XLINK_RET_ERR_IF(id == INVALID_STREAM_ID, NULL);
    XLINK_RET_ERR_IF(link == NULL, NULL);
    int stream;
    for (stream = 0; stream < XLINK_MAX_STREAMS; stream++) {
        if (link->availableStreams[stream].id == id) {
            int rc = XLink_sem_wait(&link->availableStreams[stream].sem);
            if (rc) {
                mvLog(MVLOG_ERROR,"can't wait semaphore\n");
                return NULL;
            }
            return &link->availableStreams[stream];
        }
    }
    return NULL;
}

streamDesc_t* getStreamByName(xLinkDesc_t* link, const char* name)
{
    XLINK_RET_ERR_IF(link == NULL, NULL);
    int stream;
    for (stream = 0; stream < XLINK_MAX_STREAMS; stream++) {
        if (link->availableStreams[stream].id != INVALID_STREAM_ID &&
            strcmp(link->availableStreams[stream].name, name) == 0) {
            int rc = XLink_sem_wait(&link->availableStreams[stream].sem);
            if (rc) {
                mvLog(MVLOG_ERROR,"can't wait semaphore\n");
                return NULL;
            }
            return &link->availableStreams[stream];
        }
    }
    return NULL;
}

void releaseStream(streamDesc_t* stream)
{
    if (stream && stream->id != INVALID_STREAM_ID) {
        XLink_sem_post(&stream->sem);
    }
    else {
        mvLog(MVLOG_DEBUG,"trying to release a semaphore for a released stream\n");
    }
}

xLinkState_t getXLinkState(xLinkDesc_t* link)
{
    XLINK_RET_ERR_IF(link == NULL, XLINK_NOT_INIT);
    mvLog(MVLOG_DEBUG,"%s() link %p link->peerState %d\n", __func__,link, link->peerState);
    return link->peerState;
}
