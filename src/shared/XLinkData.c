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

#ifdef __unix__
#include <sys/stat.h>
#endif

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static XLinkError_t checkEventHeader(xLinkEventHeader_t header);
static float timespec_diff(struct timespec *start, struct timespec *stop);
static XLinkError_t addEvent(xLinkEvent_t *event, unsigned int timeoutMs);
static XLinkError_t addEvent_(xLinkEvent_t *event, unsigned int timeoutMs, XLinkTimespec* outTime);
static XLinkError_t addEventWithPerf(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs);
static XLinkError_t addEventWithPerf_(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs, XLinkTimespec* outTime);
static XLinkError_t addEventWithPerfTimeout(xLinkEvent_t *event, float* opTime, unsigned int msTimeout);
static XLinkError_t getLinkByStreamId(XLinkHandler_t* handler, streamId_t streamId, xLinkDesc_t** out_link);

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------

streamId_t XLinkOpenStream(XLinkHandler_t* handler, const char* name, int stream_write_size)
{
    XLINK_RET_ERR_IF(name == NULL, INVALID_STREAM_ID);
    XLINK_RET_ERR_IF(stream_write_size < 0, INVALID_STREAM_ID);

    xLinkDesc_t* link = getLink(handler);
    mvLog(MVLOG_DEBUG,"%s() handler %p link %p\n", __func__, handler, link);
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
    }
    streamId_t streamId = getStreamIdByName(link, name);

    if (streamId > 0x0FFFFFFF) {
        mvLog(MVLOG_ERROR, "Cannot find stream id by the \"%s\" name", name);
        mvLog(MVLOG_ERROR,"Max streamId reached!");
        return INVALID_STREAM_ID;
    }
    // TODO(themarpe) - server side
    // if (streamId == INVALID_STREAM_ID) {
    //     mvLog(MVLOG_ERROR,"Max streamId reached %x!", streamId);
    //     return INVALID_STREAM_ID;
    // }

    COMBINE_IDS(streamId, link->id);
    return streamId;
}

// Just like open stream, when closeStream is called
// on the local size we are resetting the writeSize
// and on the remote side we are freeing the read buffer
XLinkError_t XLinkCloseStream(XLinkHandler_t* handler, streamId_t const streamId)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_CLOSE_STREAM_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));
    return X_LINK_SUCCESS;
}

XLinkError_t XLinkGateWrite(const char *name, void *data, int size, int timeout)
{
    int rc = XLinkPlatformGateWrite(name, data, size, timeout);
    if(rc < 0) {
        return X_LINK_ERROR;
    } else {
        return X_LINK_SUCCESS;
    }
}

XLinkError_t XLinkGateRead(const char *name, void *data, int size, int timeout)
{
    int rc = XLinkPlatformGateRead(name, data, size, timeout);
    if(rc < 0) {
        return X_LINK_ERROR;
    } else {
        return X_LINK_SUCCESS;
    }
}

XLinkError_t XLinkWriteData_(XLinkHandler_t* handler, streamId_t streamId, const uint8_t* buffer,
                            int size, XLinkTimespec* outTSend)
{
    XLINK_RET_IF(buffer == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_WRITE_REQ,
        size,(void*)buffer, link->deviceHandle);

    XLINK_RET_IF(addEventWithPerf_(&event, &opTime, XLINK_NO_RW_TIMEOUT, outTSend));

    XLinkGlobalHandlerAccumulateWrite(size, opTime);
    link->profilingData.totalWriteBytes += size;
    link->profilingData.totalWriteTime += opTime;

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteFd(XLinkHandler_t* handler, streamId_t const streamId, const long fd)
{
    return XLinkWriteFd_(handler, streamId, fd, NULL);
}

XLinkError_t XLinkWriteFd_(XLinkHandler_t* handler, streamId_t streamId, const long fd, XLinkTimespec* outTSend)
{
    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_WRITE_FD_REQ,
        sizeof(long),(void*)fd, link->deviceHandle);

    event.data2 = (void*)NULL;
    event.data2Size = -1;

    int size = sizeof(long);
#if defined(__unix__)
    if (event.deviceHandle.protocol != X_LINK_LOCAL_SHDMEM &&
	event.header.type == XLINK_WRITE_FD_REQ) {

	if (fd >= 0) {
	    // Determine file size through fstat
    	    struct stat fileStats;
	    fstat(fd, &fileStats);
	    size = fileStats.st_size;

	    if (size > 0) {
		event.header.size = size;
	    }
	}
    }
#endif

    XLINK_RET_IF(addEventWithPerf_(&event, &opTime, XLINK_NO_RW_TIMEOUT, outTSend));

    XLinkGlobalHandlerAccumulateWrite(size, opTime);
    link->profilingData.totalWriteBytes += size;
    link->profilingData.totalWriteTime += opTime;

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteFdData(XLinkHandler_t* handler, streamId_t streamId, const long fd, const uint8_t* dataBuffer, int dataSize)
{
    ASSERT_XLINK(dataBuffer);

    float opTime = 0;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId = EXTRACT_STREAM_ID(streamId);

    int totalSize = dataSize;
    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_WRITE_FD_REQ, totalSize, (void*)fd, link->deviceHandle);

    event.data2 = (void*)dataBuffer;
    event.data2Size = dataSize;

#if defined(__unix__)
    if (event.deviceHandle.protocol != X_LINK_LOCAL_SHDMEM &&
	event.header.type == XLINK_WRITE_FD_REQ) {

	if (fd >= 0) {
	    // Determine file size through fstat
    	    struct stat fileStats;
	    fstat(fd, &fileStats);
	    int size = fileStats.st_size;

	    if (size > 0) {
		event.header.size += size;
		totalSize += size;
	    }
	}
    }
#endif

    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    XLinkGlobalHandlerAccumulateWrite(totalSize, opTime);

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteData(XLinkHandler_t* handler, streamId_t const streamId, const uint8_t* buffer,
                            int size)
{
    return XLinkWriteData_(handler, streamId, buffer, size, NULL);
}

XLinkError_t XLinkWriteData2(XLinkHandler_t* handler, streamId_t streamId, const uint8_t* buffer1, int buffer1Size, const uint8_t* buffer2, int buffer2Size)
{
    ASSERT_XLINK(buffer1);
    ASSERT_XLINK(buffer2);

    float opTime = 0;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId = EXTRACT_STREAM_ID(streamId);

    int totalSize = buffer1Size + buffer2Size;
    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_WRITE_REQ, totalSize,(void*)buffer1, link->deviceHandle);
    event.data2 = (void*)buffer2;
    event.data2Size = buffer2Size;

    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    XLinkGlobalHandlerAccumulateWrite(totalSize, opTime);

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadData(XLinkHandler_t* handler, streamId_t const streamId, streamPacketDesc_t** packet)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEventWithPerf(&event, &opTime, XLINK_NO_RW_TIMEOUT));

    *packet = (streamPacketDesc_t *)event.data;
    if(*packet == NULL) {
        return X_LINK_ERROR;
    }

    XLinkGlobalHandlerAccumulateRead((*packet)->length, opTime);
    link->profilingData.totalReadBytes += (*packet)->length;
    link->profilingData.totalReadTime += opTime;


    return X_LINK_SUCCESS;
}

XLinkError_t XLinkWriteDataWithTimeout(XLinkHandler_t* handler, streamId_t const streamId, const uint8_t* buffer,
                            int size, unsigned int timeoutMs)
{
    XLINK_RET_IF(buffer == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_WRITE_REQ,
        size,(void*)buffer, link->deviceHandle);

    mvLog(MVLOG_WARN,"XLinkWriteDataWithTimeout is not fully supported yet. The XLinkWriteData method is called instead. Desired timeout = %d\n", timeoutMs);
    XLINK_RET_IF_FAIL(addEventWithPerf(&event, &opTime, timeoutMs));

    XLinkGlobalHandlerAccumulateWrite(size, opTime);
    link->profilingData.totalWriteBytes += size;
    link->profilingData.totalWriteTime += opTime;

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadDataWithTimeout(XLinkHandler_t* handler, streamId_t streamId, streamPacketDesc_t** packet, unsigned int timeoutMs)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0.0f;
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_READ_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF_FAIL(addEventWithPerf(&event, &opTime, timeoutMs));

    *packet = (streamPacketDesc_t *)event.data;
    if(*packet == NULL) {
        return X_LINK_ERROR;
    }

    XLinkGlobalHandlerAccumulateRead((*packet)->length, opTime);
    link->profilingData.totalReadBytes += (*packet)->length;
    link->profilingData.totalReadTime += opTime;

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReadMoveData(XLinkHandler_t* handler, streamId_t const streamId, streamPacketDesc_t* const packet)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0;
    xLinkDesc_t *link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
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

    XLinkGlobalHandlerAccumulateRead(packet->length, opTime);
    link->profilingData.totalReadBytes += packet->length;
    link->profilingData.totalReadTime += opTime;


    const XLinkError_t retVal = XLinkReleaseData(handler, streamId);
    if (retVal != X_LINK_SUCCESS) {
        // severe error; deallocate here as the caller might forget to dealloc on errors; or be less able to manage
        XLinkPlatformDeallocateData(packet->data, ALIGN_UP_INT32((int32_t)packet->length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
        packet->data = NULL;
        packet->length = 0;
    }
    return retVal;
}

XLinkError_t XLinkReadMoveDataWithTimeout(XLinkHandler_t* handler, streamId_t const streamId, streamPacketDesc_t* const packet, const unsigned int msTimeout)
{
    XLINK_RET_IF(packet == NULL);

    float opTime = 0;
    xLinkDesc_t *link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
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

    XLinkGlobalHandlerAccumulateRead(packet->length, opTime);
    link->profilingData.totalReadBytes += packet->length;
    link->profilingData.totalReadTime += opTime;

    const XLinkError_t retVal = XLinkReleaseData(handler, streamId);
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

XLinkError_t XLinkReleaseData(XLinkHandler_t* handler, streamId_t const streamId)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId_t streamIdOnly = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamIdOnly, XLINK_READ_REL_REQ,
        0, NULL, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));

    return X_LINK_SUCCESS;
}

XLinkError_t XLinkReleaseSpecificData(XLinkHandler_t* handler, streamId_t streamId, streamPacketDesc_t* packetDesc)
{
    xLinkDesc_t* link = NULL;
    XLINK_RET_IF(getLinkByStreamId(handler, streamId, &link));
    streamId = EXTRACT_STREAM_ID(streamId);

    xLinkEvent_t event = {0};
    XLINK_INIT_EVENT(event, streamId, XLINK_READ_REL_SPEC_REQ,
        0, (void*)packetDesc->data, link->deviceHandle);

    XLINK_RET_IF(addEvent(&event, XLINK_NO_RW_TIMEOUT));

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

XLinkError_t addEvent_(xLinkEvent_t *event, unsigned int timeoutMs, XLinkTimespec* outTime)
{
    ASSERT_XLINK(event);

    xLinkEvent_t* ev = DispatcherAddEvent_(EVENT_LOCAL, event, outTime);
    if(ev == NULL) {
        mvLog(MVLOG_ERROR, "Dispatcher failed on adding event. type: %s, id: %d, stream name: %s\n",
            TypeToStr(event->header.type), event->header.id, event->header.streamName);
        return X_LINK_ERROR;
    }

    if (timeoutMs != XLINK_NO_RW_TIMEOUT) {
        ASSERT_XLINK(event->header.type == XLINK_READ_REQ);
        if (DispatcherWaitEventComplete(&event->deviceHandle, timeoutMs))  // timeout reached
        {
            streamDesc_t* stream = getStreamById(getLinkFromDeviceHandle(&event->deviceHandle),
                                                 event->header.streamId);
            if (event->header.type == XLINK_READ_REQ)
            {
                // XLINK_READ_REQ is a local event. It is safe to serve it.
                // Limitations.
                // Possible vulnerability in this mechanism:
                //      If we reach timeout with DispatcherWaitEventComplete and before
                //      we call DispatcherServeEvent, the event actually comes,
                //      and gets served by XLink stack and event semaphore is posted.
                DispatcherServeEvent(event->header.id, XLINK_READ_REQ, stream->id, &event->deviceHandle);
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
XLinkError_t addEvent(xLinkEvent_t *event, unsigned int timeoutMs)
{
    return addEvent_(event, timeoutMs, NULL);
}

XLinkError_t addEventWithPerf_(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs, XLinkTimespec* outTime)
{
    ASSERT_XLINK(opTime);

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    XLINK_RET_IF_FAIL(addEvent_(event, timeoutMs, outTime));

    clock_gettime(CLOCK_REALTIME, &end);
    *opTime = timespec_diff(&start, &end);

    return X_LINK_SUCCESS;
}
XLinkError_t addEventWithPerf(xLinkEvent_t *event, float* opTime, unsigned int timeoutMs)
{
    return addEventWithPerf_(event, opTime, timeoutMs, NULL);
}

XLinkError_t addEventWithPerfTimeout(xLinkEvent_t *event, float* opTime, unsigned int msTimeout)
{
    ASSERT_XLINK(opTime);

    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    int rc = addEvent(event, msTimeout);
    if(rc != X_LINK_SUCCESS) return rc;

    clock_gettime(CLOCK_REALTIME, &end);
    *opTime = timespec_diff(&start, &end);

    return X_LINK_SUCCESS;
}

static XLinkError_t getLinkByStreamId(XLinkHandler_t* handler, streamId_t streamId, xLinkDesc_t** out_link) {
    ASSERT_XLINK(out_link != NULL);

    *out_link = getLink(handler);
    XLINK_RET_ERR_IF(*out_link == NULL, X_LINK_ERROR);
    XLINK_RET_ERR_IF(getXLinkState(*out_link) != XLINK_UP,
                    X_LINK_COMMUNICATION_NOT_OPEN);

    return X_LINK_SUCCESS;
}
// ------------------------------------
// Helpers declaration. End.
// ------------------------------------
