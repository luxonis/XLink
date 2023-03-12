// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "string.h"
#include "stdlib.h"
#include "time.h"

#if (defined(_WIN32) || defined(_WIN64))
#include "win_time.h"
#endif

#include "XLink.h"
#include "XLinkErrorUtils.h"

#include "XLinkMacros.h"
#include "XLinkPrivateFields.h"
#include "XLinkPlatform.h"

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif

#include "XLinkLog.h"
#include "XLinkStringUtils.h"

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

#ifndef __DEVICE__
static XLinkError_t checkEventHeader(xLinkEventHeader_t header);
#endif

static float timespec_diff(struct timespec *start, struct timespec *stop);
static XLinkError_t addEvent(xLinkEvent_t *event, unsigned int timeoutMs);
static XLinkError_t addEventWithPerf(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs);
static XLinkError_t addEventWithPerfTimeout(xLinkEvent_t *event, float* opTime, unsigned int msTimeout);
static XLinkError_t getLinkByStreamId(streamId_t streamId, xLinkDesc_t** out_link);

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------

streamId_t XLinkOpenStream(linkId_t id, const char* name, int stream_write_size)
{
    XLINK_RET_ERR_IF(name == NULL, INVALID_STREAM_ID);
    XLINK_RET_ERR_IF(stream_write_size < 0, INVALID_STREAM_ID);

    xLinkDesc_t* link = getLinkById(id);
    mvLog(MVLOG_DEBUG,"%s() id %d link %p\n", __func__, id, link);
    XLINK_RET_ERR_IF(link == NULL, INVALID_STREAM_ID);
    XLINK_RET_ERR_IF(getXLinkState(link) != XLINK_UP, INVALID_STREAM_ID);
    XLINK_RET_ERR_IF(strlen(name) >= MAX_STREAM_NAME_LENGTH, INVALID_STREAM_ID);

    if(stream_write_size > 0)
    {
        stream_write_size = ALIGN_UP(stream_write_size, __CACHE_LINE_SIZE);

        xLinkEvent_t event = {0};
        XLINK_INIT_EVENT(event, INVALID_STREAM_ID, XLINK_CREATE_STREAM_REQ,
                         stream_write_size, NULL, link->deviceHandle);
        mv_strncpy(event.header.streamName, MAX_STREAM_NAME_LENGTH,
                   name, MAX_STREAM_NAME_LENGTH - 1);

        DispatcherAddEvent(EVENT_LOCAL, &event);
        XLINK_RET_ERR_IF(
            DispatcherWaitEventComplete(&link->deviceHandle, XLINK_NO_RW_TIMEOUT),
            INVALID_STREAM_ID);

#ifndef __DEVICE__
        XLinkError_t eventStatus = checkEventHeader(event.header);
        if (eventStatus != X_LINK_SUCCESS) {
            mvLog(MVLOG_ERROR, "Got wrong package from device, error code = %s", XLinkErrorToStr(eventStatus));
            // FIXME: not good solution, but seems the only in the case of such XLink API
            if (eventStatus == X_LINK_OUT_OF_MEMORY) {
                return INVALID_STREAM_ID_OUT_OF_MEMORY;
            } else {
                return INVALID_STREAM_ID;
            }
        }
#endif
    }
    streamId_t streamId = getStreamIdByName(link, name);

#ifndef __DEVICE__
    if (streamId > 0x0FFFFFFF) {
        mvLog(MVLOG_ERROR, "Cannot find stream id by the \"%s\" name", name);
        mvLog(MVLOG_ERROR,"Max streamId reached!");
        return INVALID_STREAM_ID;
    }
#else
    if (streamId == INVALID_STREAM_ID) {
        mvLog(MVLOG_ERROR,"Max streamId reached %x!", streamId);
        return INVALID_STREAM_ID;
    }
#endif

    COMBINE_IDS(streamId, id);
    return streamId;
}

// Just like open stream, when closeStream is called
// on the local size we are resetting the writeSize
// and on the remote side we are freeing the read buffer
XLinkError_t XLinkCloseStream(streamId_t const streamId)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_CLOSE_STREAM_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteData(streamId_t const streamId, const uint8_t* buffer,
                            int size)
{
    XLINK_RET_IF(buffer == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_WRITE_REQ,
        size,(void*)buffer, link->deviceHandle);

    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    if (glHandler->profEnable) {
        glHandler->profilingData.totalWriteBytes += size;
        glHandler->profilingData.totalWriteTime += opTime;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadData(streamId_t const streamId, streamPacketDesc_t** packet)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    *packet = (streamPacketDesc_t *)event.data;
    if(*packet == NULL) {
        return X_LINK_ERROR;
    }

    if( glHandler->profEnable) {
        glHandler->profilingData.totalReadBytes += (*packet)->length;
        glHandler->profilingData.totalReadTime += opTime;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteDataWithTimeout(streamId_t const streamId, const uint8_t* buffer,
                            int size, unsigned int timeoutMs)
{
    XLINK_RET_IF(buffer == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_WRITE_REQ,
        size,(void*)buffer, link->deviceHandle);

    mvLog(MVLOG_WARN,"XLinkWriteDataWithTimeout is not fully supported yet. The XLinkWriteData method is called instead. Desired timeout = %d\n", timeoutMs);
    XLINK_RET_IF_FAIL(addEventWithPerf(&event, &opTime, timeoutMs));

    if( glHandler->profEnable) {
        glHandler->profilingData.totalWriteBytes += size;
        glHandler->profilingData.totalWriteTime += opTime;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadDataWithTimeout(streamId_t streamId, streamPacketDesc_t** packet, unsigned int timeoutMs)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_READ_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF_FAIL(addEventWithPerf(&event, &opTime, timeoutMs));

    *packet = (streamPacketDesc_t *)event.data;
    if(*packet == NULL) {
        return X_LINK_ERROR;
    }

    if( glHandler->profEnable) {
        glHandler->profilingData.totalReadBytes += (*packet)->length;
        glHandler->profilingData.totalReadTime += opTime;
    }

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadMoveData(streamId_t const streamId, streamPacketDesc_t* const packet)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0;
    xLinkDesc_t *link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REQ,
                     0, NULL, link->deviceHandle);
    event.header.flags.bitField.moveSemantic = 1;
    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    if (!event.data)
    {
        return X_LINK_ERROR;
    }
    *packet = *(streamPacketDesc_t *)event.data;

    // free the allocation from movePacketFromStream()
    // done within this same XLink module so the same C runtime is used
    free(event.data);

    if (glHandler->profEnable)
    {
        glHandler->profilingData.totalReadBytes += packet->length;
        glHandler->profilingData.totalReadTime += opTime;
    }

    const XLinkError_t retVal = XLinkReleaseData(streamId);
    if (retVal != X_LINK_SUCCESS) {
        // severe error; deallocate here as the caller might forget to dealloc on errors; or be less able to manage
        XLinkPlatformDeallocateData(packet->data, ALIGN_UP_INT32((int32_t)packet->length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
        packet->data = NULL;
        packet->length = 0;
    }
    return retVal;
}

XLinkError_t XLinkReadMoveDataWithTimeout(streamId_t const streamId, streamPacketDesc_t* const packet, const unsigned int msTimeout)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0;
    xLinkDesc_t *link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REQ,
                     0, NULL, link->deviceHandle);
    event.header.flags.bitField.moveSemantic = 1;

    const XLinkError_t rc = addEventWithPerfTimeout(&event, &opTime, msTimeout);
    if(rc == X_LINK_TIMEOUT) return rc;
    else XLINK_RET_IF(rc);

    if (!event.data)
    {
        return X_LINK_ERROR;
    }
    *packet = *(streamPacketDesc_t *)event.data;

    // free the allocation from movePacketFromStream()
    // done within this same XLink module so the same C runtime is used
    free(event.data);

    if (glHandler->profEnable)
    {
        glHandler->profilingData.totalReadBytes += packet->length;
        glHandler->profilingData.totalReadTime += opTime;
    }

    const XLinkError_t retVal = XLinkReleaseData(streamId);
    if (retVal != X_LINK_SUCCESS) {
        // severe error; deallocate here as the caller might forget to dealloc on errors; or be less able to manage
        XLinkPlatformDeallocateData(packet->data, ALIGN_UP_INT32((int32_t)packet->length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
        packet->data = NULL;
        packet->length = 0;
    }
    return retVal;
}

void XLinkDeallocateMoveData(void* const data, const uint32_t length) {
    XLinkPlatformDeallocateData(data, ALIGN_UP_INT32((int32_t)length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
}

XLinkError_t XLinkReleaseData(streamId_t const streamId)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REL_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReleaseSpecificData(streamId_t streamId, streamPacketDesc_t* packetDesc)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_READ_REL_SPEC_REQ,
        0, (void*)packetDesc->data, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkGetFillLevel(streamId_t const streamId, int isRemote, int* fillLevel)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    streamDesc_t* stream =
        getStreamById(link->deviceHandle.xLinkFD, streamIdOnly);
    ASSERT_XLINK(stream);

    if (isRemote) {
        *fillLevel = stream->remoteFillLevel;
    }
    else {
        *fillLevel = stream->localFillLevel;
    }

    releaseStream(stream);
    return X_LINK_SUCCESS;
}

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

XLinkError_t checkEventHeader(xLinkEventHeader_t header) {
    mvLog(MVLOG_DEBUG, "header.flags.bitField: ack:%u, nack:%u, sizeTooBig:%u, block:%u, bufferFull:%u, localServe:%u, noSuchStream:%u, terminate:%u",
          header.flags.bitField.ack,
          header.flags.bitField.nack,
          header.flags.bitField.sizeTooBig,
          header.flags.bitField.block,
          header.flags.bitField.bufferFull,
          header.flags.bitField.localServe,
          header.flags.bitField.noSuchStream,
          header.flags.bitField.terminate);


    if (header.flags.bitField.ack) {
        return X_LINK_SUCCESS;
    } else if (header.flags.bitField.nack) {
        return X_LINK_COMMUNICATION_FAIL;
    } else if (header.flags.bitField.sizeTooBig) {
        return X_LINK_OUT_OF_MEMORY;
    } else {
        return X_LINK_ERROR;
    }
}

float timespec_diff(struct timespec *start, struct timespec *stop)
{
    if ((stop->tv_nsec - start->tv_nsec) < 0) {
        start->tv_sec = stop->tv_sec - start->tv_sec - 1;
        start->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
    } else {
        start->tv_sec = stop->tv_sec - start->tv_sec;
        start->tv_nsec = stop->tv_nsec - start->tv_nsec;
    }

    return start->tv_nsec/ 1000000000.0f + start->tv_sec;
}

XLinkError_t addEvent(xLinkEvent_t *event, unsigned int timeoutMs)
{
    ASSERT_XLINK(event);

    xLinkEvent_t* ev = DispatcherAddEvent(EVENT_LOCAL, event);
    if(ev == NULL) {
        mvLog(MVLOG_ERROR, "Dispatcher failed on adding event. type: %s, id: %d, stream name: %s\n",
            TypeToStr(event->header.type), event->header.id, event->header.streamName);
        return X_LINK_ERROR;
    }

    if (timeoutMs != XLINK_NO_RW_TIMEOUT) {
        ASSERT_XLINK(event->header.type == XLINK_READ_REQ);
        xLinkDesc_t* link;
        getLinkByStreamId(event->header.streamId, &link);

        if (DispatcherWaitEventComplete(&event->deviceHandle, timeoutMs))  // timeout reached
        {
            streamDesc_t* stream = getStreamById(event->deviceHandle.xLinkFD,
                                                 event->header.streamId);
            if (event->header.type == XLINK_READ_REQ)
            {
                // XLINK_READ_REQ is a local event. It is safe to serve it.
                // Limitations.
                // Possible vulnerability in this mechanism:
                //      If we reach timeout with DispatcherWaitEventComplete and before
                //      we call DispatcherServeEvent, the event actually comes,
                //      and gets served by XLink stack and event semaphore is posted.
                DispatcherServeEvent(event->header.id, XLINK_READ_REQ, stream->id, event->deviceHandle.xLinkFD);
            }
            releaseStream(stream);

            return X_LINK_TIMEOUT;
        }
    }
    else  // No timeout
    {
        if (DispatcherWaitEventComplete(&event->deviceHandle, timeoutMs))
        {
            return X_LINK_TIMEOUT;
        }
    }

    XLINK_RET_ERR_IF(
        event->header.flags.bitField.ack != 1,
        X_LINK_COMMUNICATION_FAIL);

    return X_LINK_SUCCESS;
}

XLinkError_t addEventWithPerf(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs)
{
    ASSERT_XLINK(opTime);

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    XLINK_RET_IF_FAIL(addEvent(event, timeoutMs));

    clock_gettime(CLOCK_REALTIME, &end);
    *opTime = timespec_diff(&start, &end);

    return X_LINK_SUCCESS;
}

XLinkError_t addEventTimeout(xLinkEvent_t *event, struct timespec abstime)
{
    ASSERT_XLINK(event);

    xLinkEvent_t* ev = DispatcherAddEvent(EVENT_LOCAL, event);
    if(ev == NULL) {
        mvLog(MVLOG_ERROR, "Dispatcher failed on adding event. type: %s, id: %d, stream name: %s\n",
            TypeToStr(event->header.type), event->header.id, event->header.streamName);
        return X_LINK_ERROR;
    }

    if (DispatcherWaitEventCompleteTimeout(&event->deviceHandle, abstime)) {
        return X_LINK_TIMEOUT;
    }

    XLINK_RET_ERR_IF(
        event->header.flags.bitField.ack != 1,
        X_LINK_COMMUNICATION_FAIL);

    return X_LINK_SUCCESS;
}

XLinkError_t addEventWithPerfTimeout(xLinkEvent_t *event, float* opTime, unsigned int msTimeout)
{
    ASSERT_XLINK(opTime);

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    struct timespec absTimeout = start;
    int64_t sec = msTimeout / 1000;
    absTimeout.tv_sec += sec;
    absTimeout.tv_nsec += (long)((msTimeout - (sec * 1000)) * 1000000);
    int64_t secOver = absTimeout.tv_nsec / 1000000000;
    absTimeout.tv_nsec -= (long)(secOver * 1000000000);
    absTimeout.tv_sec += secOver;

    int rc = addEventTimeout(event, absTimeout);
    if(rc != X_LINK_SUCCESS) return rc;

    clock_gettime(CLOCK_REALTIME, &end);
    *opTime = timespec_diff(&start, &end);

    return X_LINK_SUCCESS;
}

static XLinkError_t getLinkByStreamId(streamId_t streamId, xLinkDesc_t** out_link) {
    ASSERT_XLINK(out_link != NULL);

    linkId_t id = EXTRACT_LINK_ID(streamId);
    *out_link = getLinkById(id);

    XLINK_RET_ERR_IF(*out_link == NULL, X_LINK_ERROR);
    XLINK_RET_ERR_IF(getXLinkState(*out_link) != XLINK_UP,
                    X_LINK_COMMUNICATION_NOT_OPEN);

    return X_LINK_SUCCESS;
}
// ------------------------------------
// Helpers declaration. End.
// ------------------------------------
