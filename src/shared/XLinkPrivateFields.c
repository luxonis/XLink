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

xLinkDesc_t* getLinkById(linkId_t id)
{
    if (id == INVALID_LINK_ID) return NULL;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&availableXLinksMutex) != 0, NULL);

    int i;
    for (i = 0; i < MAX_LINKS; i++) {
        if (availableXLinks[i].id == id) {
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
            return &availableXLinks[i];
        }
    }

    XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
    return NULL;
}

xLinkDesc_t* getLink(void* fd)
{
    if (fd == NULL) return NULL;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&availableXLinksMutex) != 0, NULL);

    int i;
    for (i = 0; i < MAX_LINKS; i++) {
        if (availableXLinks[i].id != INVALID_LINK_ID && availableXLinks[i].deviceHandle.xLinkFD == fd) {
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
            return &availableXLinks[i];
        }
    }

    XLINK_RET_ERR_IF(pthread_mutex_unlock(&availableXLinksMutex) != 0, NULL);
    return NULL;
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

streamDesc_t* getStreamById(void* fd, streamId_t id)
{
    XLINK_RET_ERR_IF(id == INVALID_STREAM_ID, NULL);
    xLinkDesc_t* link = getLink(fd);
    XLINK_RET_ERR_IF(link == NULL, NULL);
    streamDesc_t* stream = NULL;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&availableXLinksMutex), NULL);
    linkId_t linkId = link->id;
    if (linkId == INVALID_LINK_ID || link->deviceHandle.xLinkFD != fd) {
        pthread_mutex_unlock(&availableXLinksMutex);
        return NULL;
    }
    for (int index = 0; index < XLINK_MAX_STREAMS; ++index) {
        if (link->availableStreams[index].id == id) {
            stream = &link->availableStreams[index];
            break;
        }
    }
    pthread_mutex_unlock(&availableXLinksMutex);
    if (!stream || XLink_sem_wait(&stream->sem)) return NULL;
    if (pthread_mutex_lock(&availableXLinksMutex)) {
        releaseStream(stream);
        return NULL;
    }
    int current = link->id == linkId && link->deviceHandle.xLinkFD == fd && stream->id == id;
    pthread_mutex_unlock(&availableXLinksMutex);
    if (current) return stream;
    releaseStream(stream);
    return NULL;
}

streamDesc_t* getStreamByName(xLinkDesc_t* link, const char* name)
{
    XLINK_RET_ERR_IF(link == NULL, NULL);
    streamDesc_t* stream = NULL;
    streamId_t id = INVALID_STREAM_ID;
    XLINK_RET_ERR_IF(pthread_mutex_lock(&availableXLinksMutex), NULL);
    linkId_t linkId = link->id;
    void* fd = link->deviceHandle.xLinkFD;
    if (linkId == INVALID_LINK_ID || fd == NULL) {
        pthread_mutex_unlock(&availableXLinksMutex);
        return NULL;
    }
    for (int index = 0; index < XLINK_MAX_STREAMS; ++index) {
        streamDesc_t* candidate = &link->availableStreams[index];
        if (candidate->id != INVALID_STREAM_ID && strcmp(candidate->name, name) == 0) {
            stream = candidate;
            id = candidate->id;
            break;
        }
    }
    pthread_mutex_unlock(&availableXLinksMutex);
    if (!stream || XLink_sem_wait(&stream->sem)) return NULL;
    if (pthread_mutex_lock(&availableXLinksMutex)) {
        releaseStream(stream);
        return NULL;
    }
    int current = link->id == linkId && link->deviceHandle.xLinkFD == fd
        && stream->id == id && strcmp(stream->name, name) == 0;
    pthread_mutex_unlock(&availableXLinksMutex);
    if (current) return stream;
    releaseStream(stream);
    return NULL;
}

void releaseStream(streamDesc_t* stream)
{
    if (stream) XLink_sem_post(&stream->sem);
}

xLinkState_t getXLinkState(xLinkDesc_t* link)
{
    XLINK_RET_ERR_IF(link == NULL, XLINK_NOT_INIT);
    mvLog(MVLOG_DEBUG,"%s() link %p link->peerState %d\n", __func__,link, link->peerState);
    return link->peerState;
}
