// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>
#include "stdlib.h"

#include "XLinkMacros.h"
#include "XLinkErrorUtils.h"
#include "XLinkPlatform.h"
#include "XLinkDispatcherImpl.h"
#include "XLinkPrivateFields.h"

#include "XLinkTime.h"

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif
#include "XLinkLog.h"
#include "XLinkStringUtils.h"

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static int isStreamSpaceEnoughFor(streamDesc_t* stream, uint32_t size);

// moves packet and its data out of XLink; caller is responsible for freeing data resource
static streamPacketDesc_t* movePacketFromStream(streamDesc_t *stream);
static streamPacketDesc_t* getPacketFromStream(streamDesc_t* stream);
static int releasePacketFromStream(streamDesc_t* stream, uint32_t* releasedSize);
static int releaseSpecificPacketFromStream(streamDesc_t* stream, uint32_t* releasedSize, uint8_t* data);
static int addNewPacketToStream(streamDesc_t* stream, void* buffer, uint32_t size, long fd, XLinkTimespec trsend, XLinkTimespec treceive);

static int handleIncomingEvent(xLinkEvent_t* event, XLinkTimespec treceive);

// ------------------------------------
// Helpers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkDispatcherImpl.h implementation. Begin.
// ------------------------------------

int writeEventMultipart(xLinkDeviceHandle_t* deviceHandle, void* data, int totalSize, void* data2, int data2Size)
{
    // Regular, single-part case
    if(data2 == NULL || data2Size <= 0) {
        return XLinkPlatformWrite(deviceHandle, data, totalSize);
    }

    // Multipart case
    int errorCode = 0;
    void *dataToWrite[] = {data, data2, NULL};
    int sizeToWrite[] = {totalSize - data2Size, data2Size, 0};

    int writtenByteCount = 0, toWrite = 0, rc = 0;

    int totalSizeToWrite = 0;

    int pktlen = 0;

    // restriction on the output data size
    // mitigates kernel crash on RPI when USB is used
    const int xlinkPacketSizeMultiply = deviceHandle->protocol == X_LINK_USB_VSC ? 1024 : 1; //for usb3, usb2 is 512
    uint8_t swapSpaceScratchBufferVsc[1024 + 64];
    uint8_t swapSpaceScratchBuffer[1 + 64];
    uint8_t* swapSpace = swapSpaceScratchBuffer + ALIGN_UP((((uintptr_t)swapSpaceScratchBuffer) % 64), 64);
    if(deviceHandle->protocol == X_LINK_USB_VSC) {
        swapSpace = swapSpaceScratchBufferVsc + ALIGN_UP((((uintptr_t)swapSpaceScratchBufferVsc) % 64), 64);
    }

    // the amount of bytes written from split transfer for "next" packet
    int previousSplitWriteSize = 0;
    for (int i = 0;; i++) {
        void *currentPacket = dataToWrite[i];
        int currentPacketSize = sizeToWrite[i];
        if (currentPacket == NULL) break;
        if (currentPacketSize == 0) break;
        // printf("currentPacket %p size %d \n", currentPacket, currentPacketSize);
        void *nextPacket = dataToWrite[i + 1];
        int nextPacketSize = sizeToWrite[i + 1];
        bool shouldSplitData = false;

        if (nextPacket != NULL && nextPacketSize > 0) {
            totalSizeToWrite += currentPacketSize - (currentPacketSize % xlinkPacketSizeMultiply);
            if(currentPacketSize % xlinkPacketSizeMultiply) {
                shouldSplitData = true;
            }
        } else {
            totalSizeToWrite += currentPacketSize;
        }

        // printf("writtenByteCount %d %d\n",writtenByteCount , totalSizeToWrite);
        int byteCountRelativeOffset = writtenByteCount;
        while (writtenByteCount < totalSizeToWrite) {
            toWrite = (pktlen && (totalSizeToWrite - writtenByteCount) > pktlen)
                          ? pktlen
                          : (totalSizeToWrite - writtenByteCount);

            rc = XLinkPlatformWrite(deviceHandle, &((char *)currentPacket)[writtenByteCount - byteCountRelativeOffset + previousSplitWriteSize], toWrite);
            if (rc < 0)
            {
                errorCode = rc;
                goto function_epilogue;
            }
            writtenByteCount += toWrite;
        }
        if (shouldSplitData) {
            int remainingToWriteCurrent = currentPacketSize - (totalSizeToWrite - byteCountRelativeOffset);
            // printf("remainingToWriteCurrent %d \n", remainingToWriteCurrent);
            if(remainingToWriteCurrent < 0 || remainingToWriteCurrent > xlinkPacketSizeMultiply) ASSERT_XLINK(0);
            int remainingToWriteNext = nextPacketSize > xlinkPacketSizeMultiply - remainingToWriteCurrent ? xlinkPacketSizeMultiply - remainingToWriteCurrent : nextPacketSize;
            // printf("remainingToWriteNext %d \n", remainingToWriteNext);
            if(remainingToWriteNext < 0 || remainingToWriteNext > xlinkPacketSizeMultiply) ASSERT_XLINK(0);

            if (remainingToWriteCurrent) {
                memcpy(swapSpace, &((char *)currentPacket)[writtenByteCount - byteCountRelativeOffset + previousSplitWriteSize], remainingToWriteCurrent);
                if(remainingToWriteNext) {
                    memcpy(swapSpace + remainingToWriteCurrent, nextPacket, remainingToWriteNext);
                }
                toWrite = remainingToWriteCurrent + remainingToWriteNext;
                if(toWrite > xlinkPacketSizeMultiply) ASSERT_XLINK(0);
                rc = XLinkPlatformWrite(deviceHandle, swapSpace, toWrite);
                if (rc < 0)
                {
                    errorCode = rc;
                    goto function_epilogue;
                }
                writtenByteCount += toWrite;
                totalSizeToWrite += remainingToWriteCurrent;
                // printf("%s wrote %d \n", __FUNCTION__, rc);

                previousSplitWriteSize = remainingToWriteNext;
            }
        } else {
            previousSplitWriteSize = 0;
        }
    }

function_epilogue:
    if (errorCode) return errorCode;
    return writtenByteCount;
}

int writeFdEventMultipart(xLinkDeviceHandle_t* deviceHandle, void* data, int totalSize, void* data2, int data2Size)
{
    if(XLinkPlatformWriteFd(deviceHandle, data) != X_LINK_SUCCESS) {
	return X_LINK_ERROR;
    }
 
    if(data2 != NULL || data2Size > 0) {
	return XLinkPlatformWrite(deviceHandle, data2, data2Size);
    }           

    return X_LINK_SUCCESS;
}

//adds a new event with parameters and returns event id
int dispatcherEventSend(xLinkEvent_t *event, XLinkTimespec* sendTime)
{
    mvLog(MVLOG_DEBUG, "Send event: %s, size %d, streamId %ld.\n",
        TypeToStr(event->header.type), event->header.size, event->header.streamId);

    XLinkTimespec stime;
    getMonotonicTimestamp(&stime);
    if (sendTime != NULL) *sendTime = stime;

    event->header.tsecLsb = (uint32_t)stime.tv_sec;
    event->header.tsecMsb = (uint32_t)(stime.tv_sec >> 32);
    event->header.tnsec = (uint32_t)stime.tv_nsec;
    int rc = XLinkPlatformWrite(&event->deviceHandle,
        &event->header, sizeof(event->header));

    if(rc < 0) {
        mvLog(MVLOG_ERROR,"Write failed (header) (err %d) | event %s\n", rc, TypeToStr(event->header.type));
        return rc;
    }

    if (event->header.type == XLINK_WRITE_REQ) {
        rc = writeEventMultipart(&event->deviceHandle, event->data, event->header.size, event->data2, event->data2Size);
        if(rc < 0) {
            mvLog(MVLOG_ERROR,"Write failed %d\n", rc);
            return rc;
        }
    } else if (event->header.type == XLINK_WRITE_FD_REQ) {
        rc = writeFdEventMultipart(&event->deviceHandle, event->data, event->header.size, event->data2, event->data2Size);
        if(rc < 0) {
            mvLog(MVLOG_ERROR,"Write failed %d\n", rc);
            return rc;
        }   
    }

    return 0;
}

int dispatcherEventReceive(xLinkEvent_t* event){
    // static xLinkEvent_t prevEvent = {0};
    long fd = -1;
    int rc = XLinkPlatformRead(&event->deviceHandle,
        &event->header, sizeof(event->header), &fd);
    (void)fd;
    XLinkTimespec treceive;
    getMonotonicTimestamp(&treceive);

    // mvLog(MVLOG_DEBUG,"Incoming event %p: %s %d %p prevEvent: %s %d %p\n",
    //       event,
    //       TypeToStr(event->header.type),
    //       (int)event->header.id,
    //       event->deviceHandle.xLinkFD,
    //       TypeToStr(prevEvent.header.type),
    //       (int)prevEvent.header.id,
    //       prevEvent.deviceHandle.xLinkFD);

    if(rc < 0) {
        mvLog(MVLOG_WARN,"%s() Read failed %d\n", __func__, (int)rc);
        return rc;
    }

    // TODO(themarpe) - reimplement duplicate ID detection
    // if (prevEvent.header.id == event->header.id &&
    //     prevEvent.header.type == event->header.type &&
    //     prevEvent.deviceHandle.xLinkFD == event->deviceHandle.xLinkFD) {
    //     mvLog(MVLOG_FATAL,"Duplicate id detected. \n");
    // }
    // prevEvent = *event;

    return handleIncomingEvent(event, treceive);
}

//this function should be called only for local requests
int dispatcherLocalEventGetResponse(xLinkEvent_t* event, xLinkEvent_t* response, bool server)
{
    streamDesc_t* stream;
    response->header.id = event->header.id;
    response->header.tsecLsb = event->header.tsecLsb;
    response->header.tsecMsb = event->header.tsecMsb;
    response->header.tnsec = event->header.tnsec;
    mvLog(MVLOG_DEBUG, "%s\n",TypeToStr(event->header.type));
    switch (event->header.type){
        case XLINK_WRITE_REQ:
	case XLINK_WRITE_FD_REQ:
        {
            //in case local tries to write after it issues close (writeSize is zero)
            stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);

            if(!stream) {
                mvLog(MVLOG_DEBUG, "stream %d has been closed!\n", event->header.streamId);
                XLINK_SET_EVENT_FAILED_AND_SERVE(event);
                break;
            }

            if (stream->writeSize == 0)
            {
                XLINK_EVENT_NOT_ACKNOWLEDGE(event);
                // return -1 to don't even send it to the remote
                releaseStream(stream);
                return -1;
            }
            XLINK_EVENT_ACKNOWLEDGE(event);
            event->header.flags.bitField.localServe = 0;

            if(!isStreamSpaceEnoughFor(stream, event->header.size)){
                mvLog(MVLOG_DEBUG,"local NACK RTS. stream '%s' is full (event %d)\n", stream->name, event->header.id);
                event->header.flags.bitField.block = 1;
                event->header.flags.bitField.localServe = 1;
                // TODO: easy to implement non-blocking read here, just return nack
                mvLog(MVLOG_WARN, "Blocked event would cause dispatching thread to wait on semaphore infinitely\n");
            }else{
                event->header.flags.bitField.block = 0;
                stream->remoteFillLevel += event->header.size;
                stream->remoteFillPacketLevel++;
                mvLog(MVLOG_DEBUG,"S%d: Got local write of %ld , remote fill level %ld out of %ld %ld\n",
                      event->header.streamId, event->header.size, stream->remoteFillLevel, stream->writeSize, stream->readSize);
            }
            releaseStream(stream);
            break;
        }
        case XLINK_READ_REQ:
        {
            stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);
            if(!stream) {
                mvLog(MVLOG_DEBUG, "stream %d has been closed!\n", event->header.streamId);
                XLINK_SET_EVENT_FAILED_AND_SERVE(event);
                break;
            }

            streamPacketDesc_t* packet;
            if (event->header.flags.bitField.moveSemantic) {
                packet = movePacketFromStream(stream);
            }
            else {
                packet = getPacketFromStream(stream);
            }

            if (packet){
                //the read can be served with this packet
                event->data = packet;
                XLINK_EVENT_ACKNOWLEDGE(event);
                event->header.flags.bitField.block = 0;
            }
            else{
                event->header.flags.bitField.block = 1;
                // TODO: easy to implement non-blocking read here, just return nack
            }
            event->header.flags.bitField.localServe = 1;
            releaseStream(stream);
            break;
        }
        case XLINK_READ_REL_REQ:
        {
            stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);
            ASSERT_XLINK(stream);
            XLINK_EVENT_ACKNOWLEDGE(event);
            uint32_t releasedSize = 0;
            releasePacketFromStream(stream, &releasedSize);
            event->header.size = releasedSize;
            releaseStream(stream);
            break;
        }
        case XLINK_READ_REL_SPEC_REQ:
        {
            uint8_t* data = (uint8_t*)event->data;
            stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);
            ASSERT_XLINK(stream);
            XLINK_EVENT_ACKNOWLEDGE(event);
            uint32_t releasedSize = 0;
            releaseSpecificPacketFromStream(stream, &releasedSize, data);
            event->header.size = releasedSize;
            releaseStream(stream);
            break;
        }
        case XLINK_CREATE_STREAM_REQ:
        {
            XLINK_EVENT_ACKNOWLEDGE(event);

            if(!server) {
                event->header.streamId = XLinkAddOrUpdateStream(event->deviceHandle.xLinkFD,
                                                                event->header.streamName,
                                                                event->header.size, 0,
                                                                INVALID_STREAM_ID);
                mvLog(MVLOG_DEBUG, "XLINK_CREATE_STREAM_REQ - stream has been just opened with id %ld\n",
                    event->header.streamId);
            } else {
                mvLog(MVLOG_DEBUG, "XLINK_CREATE_STREAM_REQ - do nothing. Stream will be "
                    "opened with forced id accordingly to response from the host\n");
            }
            break;
        }
        case XLINK_CLOSE_STREAM_REQ:
        {
            stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);

            ASSERT_XLINK(stream);
            XLINK_EVENT_ACKNOWLEDGE(event);
            if (stream->remoteFillLevel != 0){
                stream->closeStreamInitiated = 1;
                event->header.flags.bitField.block = 1;
                event->header.flags.bitField.localServe = 1;
            }else{
                event->header.flags.bitField.block = 0;
                event->header.flags.bitField.localServe = 0;
            }
            releaseStream(stream);
            break;
        }
        case XLINK_RESET_REQ:
        {
            XLINK_EVENT_ACKNOWLEDGE(event);
            mvLog(MVLOG_DEBUG,"XLINK_RESET_REQ - do nothing\n");
            break;
        }
        case XLINK_PING_REQ:
        {
            XLINK_EVENT_ACKNOWLEDGE(event);
            mvLog(MVLOG_DEBUG,"XLINK_PING_REQ - do nothing\n");
            break;
        }
	case XLINK_WRITE_RESP:
	case XLINK_WRITE_FD_RESP:
        case XLINK_READ_RESP:
        case XLINK_READ_REL_RESP:
        case XLINK_READ_REL_SPEC_RESP:
        case XLINK_CREATE_STREAM_RESP:
        case XLINK_CLOSE_STREAM_RESP:
        case XLINK_PING_RESP:
            break;
        case XLINK_RESET_RESP:
            //should not happen
            event->header.flags.bitField.localServe = 1;
            break;
        default:
        {
            mvLog(MVLOG_ERROR,
                  "Fail to get response for local event. type: %d, stream name: %s\n",
                  event->header.type, event->header.streamName);
            ASSERT_XLINK(0);
        }
    }
    return 0;
}

//this function should be called only for remote requests
int dispatcherRemoteEventGetResponse(xLinkEvent_t* event, xLinkEvent_t* response, bool server)
{
    response->header.id = event->header.id;
    response->header.flags.raw = 0;
    response->header.tsecLsb = event->header.tsecLsb;
    response->header.tsecMsb = event->header.tsecMsb;
    response->header.tnsec = event->header.tnsec;
    mvLog(MVLOG_DEBUG, "%s\n",TypeToStr(event->header.type));

    switch (event->header.type)
    {
	case XLINK_WRITE_FD_REQ:
	    {
                //let remote write immediately as we have a local buffer for the data
                response->header.type = XLINK_WRITE_FD_RESP;
                response->header.size = event->header.size;
                response->header.streamId = event->header.streamId;
                response->deviceHandle = event->deviceHandle;
                XLINK_EVENT_ACKNOWLEDGE(response);

                // we got some data. We should unblock a blocked read
                int xxx = DispatcherUnblockEvent(-1,
                                                XLINK_READ_REQ,
                                                response->header.streamId,
                                                event->deviceHandle.xLinkFD);
                (void) xxx;
                mvLog(MVLOG_DEBUG,"unblocked from stream %d %d\n",
                    (int)response->header.streamId, (int)xxx);
            }
	    break;
        case XLINK_WRITE_REQ:
            {
                //let remote write immediately as we have a local buffer for the data
                response->header.type = XLINK_WRITE_RESP;
                response->header.size = event->header.size;
                response->header.streamId = event->header.streamId;
                response->deviceHandle = event->deviceHandle;
                XLINK_EVENT_ACKNOWLEDGE(response);

                // we got some data. We should unblock a blocked read
                int xxx = DispatcherUnblockEvent(-1,
                                                XLINK_READ_REQ,
                                                response->header.streamId,
                                                event->deviceHandle.xLinkFD);
                (void) xxx;
                mvLog(MVLOG_DEBUG,"unblocked from stream %d %d\n",
                    (int)response->header.streamId, (int)xxx);
            }
            break;
        case XLINK_READ_REQ:
            break;
        case XLINK_READ_REL_SPEC_REQ:
            XLINK_EVENT_ACKNOWLEDGE(response);
            response->header.type = XLINK_READ_REL_SPEC_RESP;
            response->deviceHandle = event->deviceHandle;
            streamDesc_t* stream = getStreamById(event->deviceHandle.xLinkFD,
                                   event->header.streamId);
            ASSERT_XLINK(stream);
            stream->remoteFillLevel -= event->header.size;
            stream->remoteFillPacketLevel--;

            mvLog(MVLOG_DEBUG,"S%d: Got remote release of %ld, remote fill level %ld out of %ld %ld\n",
                  event->header.streamId, event->header.size, stream->remoteFillLevel, stream->writeSize, stream->readSize);
            releaseStream(stream);

            DispatcherUnblockEvent(-1, XLINK_WRITE_REQ, event->header.streamId,
                                   event->deviceHandle.xLinkFD);
            //with every released packet check if the stream is already marked for close
            if (stream->closeStreamInitiated && stream->localFillLevel == 0)
            {
                mvLog(MVLOG_DEBUG,"%s() Unblock close STREAM\n", __func__);
                DispatcherUnblockEvent(-1,
                                       XLINK_CLOSE_STREAM_REQ,
                                       event->header.streamId,
                                       event->deviceHandle.xLinkFD);
            }
            break;
        case XLINK_READ_REL_REQ:
        {
            XLINK_EVENT_ACKNOWLEDGE(response);
            response->header.type = XLINK_READ_REL_RESP;
            response->deviceHandle = event->deviceHandle;
            streamDesc_t *stream = getStreamById(event->deviceHandle.xLinkFD,
                                                 event->header.streamId);
            ASSERT_XLINK(stream);
            stream->remoteFillLevel -= event->header.size;
            stream->remoteFillPacketLevel--;

            mvLog(MVLOG_DEBUG,"S%d: Got remote release of %ld, remote fill level %ld out of %ld %ld\n",
                  event->header.streamId, event->header.size, stream->remoteFillLevel, stream->writeSize, stream->readSize);
            releaseStream(stream);

            DispatcherUnblockEvent(-1, XLINK_WRITE_REQ, event->header.streamId,
                                   event->deviceHandle.xLinkFD);
            //with every released packet check if the stream is already marked for close
            if (stream->closeStreamInitiated && stream->localFillLevel == 0)
            {
                mvLog(MVLOG_DEBUG,"%s() Unblock close STREAM\n", __func__);
                int xxx = DispatcherUnblockEvent(-1,
                                                 XLINK_CLOSE_STREAM_REQ,
                                                 event->header.streamId,
                                                 event->deviceHandle.xLinkFD);
                (void) xxx;
            }
            break;
        }
        case XLINK_CREATE_STREAM_REQ:
            XLINK_EVENT_ACKNOWLEDGE(response);
            response->header.type = XLINK_CREATE_STREAM_RESP;
            //write size from remote means read size for this peer
            if(server) {
                response->header.streamId = XLinkAddOrUpdateStream(event->deviceHandle.xLinkFD,
                                                                event->header.streamName,
                                                                0, event->header.size,
                                                                event->header.streamId);
            } else {
                response->header.streamId = XLinkAddOrUpdateStream(event->deviceHandle.xLinkFD,
                                                                event->header.streamName,
                                                                0, event->header.size,
                                                                INVALID_STREAM_ID);
            }
            if (response->header.streamId == INVALID_STREAM_ID) {
                response->header.flags.bitField.ack = 0;
                response->header.flags.bitField.sizeTooBig = 1;
                break;
            }

            response->deviceHandle = event->deviceHandle;
            mv_strncpy(response->header.streamName, MAX_STREAM_NAME_LENGTH,
                       event->header.streamName, MAX_STREAM_NAME_LENGTH - 1);
            response->header.size = event->header.size;
            mvLog(MVLOG_DEBUG,"creating stream %x\n", (int)response->header.streamId);
            break;
        case XLINK_CLOSE_STREAM_REQ:
        {
            response->header.type = XLINK_CLOSE_STREAM_RESP;
            response->header.streamId = event->header.streamId;
            response->deviceHandle = event->deviceHandle;

            streamDesc_t* stream = getStreamById(event->deviceHandle.xLinkFD,
                                                 event->header.streamId);
            if (!stream) {
                //if we have sent a NACK before, when the event gets unblocked
                //the stream might already be unavailable
                XLINK_EVENT_ACKNOWLEDGE(response);
                mvLog(MVLOG_DEBUG,"%s() got a close stream on aready closed stream\n", __func__);
            } else {
                if (stream->localFillLevel == 0)
                {
                    XLINK_EVENT_ACKNOWLEDGE(response);

                    if (stream->readSize)
                    {
                        stream->readSize = 0;
                        stream->closeStreamInitiated = 0;
                    }

                    if (!stream->writeSize) {
                        stream->id = INVALID_STREAM_ID;
                        stream->name[0] = '\0';
                    }

                    // TODO(themarpe) - TBD
                    if(server) {
                        if(XLink_sem_destroy(&stream->sem))
                            perror("Can't destroy semaphore");
                    }
                }
                else
                {
                    mvLog(MVLOG_DEBUG,"%s():fifo is NOT empty returning NACK \n", __func__);
                    XLINK_EVENT_NOT_ACKNOWLEDGE(response);
                    stream->closeStreamInitiated = 1;
                }

                releaseStream(stream);
            }
            break;
        }
        case XLINK_PING_REQ:
            response->header.type = XLINK_PING_RESP;
            XLINK_EVENT_ACKNOWLEDGE(response);
            response->deviceHandle = event->deviceHandle;
            sem_post(&pingSem);
            break;
        case XLINK_RESET_REQ:
            mvLog(MVLOG_DEBUG,"reset request - received! Sending ACK *****\n");
            XLINK_EVENT_ACKNOWLEDGE(response);
            response->header.type = XLINK_RESET_RESP;
            response->deviceHandle = event->deviceHandle;
            // need to send the response, serve the event and then reset
            break;
        case XLINK_WRITE_RESP:
	    break;
	case XLINK_WRITE_FD_RESP:
            break;
        case XLINK_READ_RESP:
            break;
        case XLINK_READ_REL_RESP:
            break;
        case XLINK_READ_REL_SPEC_RESP:
            break;
        case XLINK_CREATE_STREAM_RESP:
        {
            // write_size from the response the size of the buffer from the remote
            if(server) {
                response->header.streamId = XLinkAddOrUpdateStream(event->deviceHandle.xLinkFD,
                                                                event->header.streamName,
                                                                event->header.size, 0,
                                                                event->header.streamId);
                XLINK_RET_IF(response->header.streamId
                    == INVALID_STREAM_ID);
                mvLog(MVLOG_DEBUG, "XLINK_CREATE_STREAM_REQ - stream has been just opened "
                    "with forced id=%ld accordingly to response from the host\n",
                    response->header.streamId);
            }
            response->deviceHandle = event->deviceHandle;
            break;
        }
        case XLINK_CLOSE_STREAM_RESP:
        {
            streamDesc_t* stream = getStreamById(event->deviceHandle.xLinkFD,
                                                 event->header.streamId);

            if (!stream){
                XLINK_EVENT_NOT_ACKNOWLEDGE(response);
                break;
            }
            stream->writeSize = 0;
            if (!stream->readSize) {
                XLINK_EVENT_NOT_ACKNOWLEDGE(response);
                stream->id = INVALID_STREAM_ID;
                stream->name[0] = '\0';
                break;
            }
            releaseStream(stream);
            break;
        }
        case XLINK_PING_RESP:
            break;
        case XLINK_RESET_RESP:
            break;
        default:
        {
            mvLog(MVLOG_ERROR,
                "Fail to get response for remote event. type: %d, stream name: %s\n",
                event->header.type, event->header.streamName);
            ASSERT_XLINK(0);
        }
    }
    return 0;
}

void dispatcherCloseLink(void* fd, int fullClose)
{
    xLinkDesc_t* link = getLink(fd);

    if (!link) {
        mvLog(MVLOG_WARN, "Dispatcher link is null");
        return;
    }

    // TODO investigate race condition that is (probably) later caught
    // due to changing the global `xLinkDesc_t availableXLinks[MAX_LINKS]`
    // without any thread protection. The dispatcher thread can be calling this function
    // while an app thread calls `XLinkReadData()` which calls `getLinkByStreamId()` which calls
    // both `getLinkById()` and `getXLinkState()`. The latter two read the global `availableXLinks`
    // and depending on the two threads execution timing could result in the xlink being invalidated
    // after the app's thread did the "is xlink valid" test. This leads to the app's thread
    // creating an `xLinkEvent_t` with outdated xlink info. When that event gets to the
    // event processing loop, the validity of the xlink state will be checked again and be handled
    if (!fullClose) {
        link->peerState = XLINK_DOWN;
        return;
    }

    link->id = INVALID_LINK_ID;
    link->deviceHandle.xLinkFD = NULL;
    link->peerState = XLINK_NOT_INIT;
    link->nextUniqueStreamId = 0;

    for (int index = 0; index < XLINK_MAX_STREAMS; index++) {
        streamDesc_t* stream = &link->availableStreams[index];

        // TODO integrate pending changes
        // * use move semantic, this prevents the memset(0) from causing leak/crash
        // * make new xlink-specific semaphore and wait on it during xlink lookup, create, etc.

        while (getPacketFromStream(stream) || stream->blockedPackets) {
            releasePacketFromStream(stream, NULL);
        }

        // XLink reset stream
        XLinkStreamReset(stream);
    }

    if(XLink_sem_destroy(&link->dispatcherClosedSem)) {
        mvLog(MVLOG_DEBUG, "Cannot destroy dispatcherClosedSem\n");
    }
}

void dispatcherCloseDeviceFd(xLinkDeviceHandle_t* deviceHandle)
{
    XLinkPlatformCloseRemote(deviceHandle);
}

// ------------------------------------
// XLinkDispatcherImpl.h implementation. End.
// ------------------------------------



// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------

int isStreamSpaceEnoughFor(streamDesc_t* stream, uint32_t size)
{
    if(stream->remoteFillPacketLevel >= XLINK_MAX_PACKETS_PER_STREAM ||
       stream->remoteFillLevel + size > stream->writeSize){
        mvLog(MVLOG_DEBUG, "S%d: Not enough space in stream '%s' for %ld: PKT %ld, FILL %ld SIZE %ld\n",
              stream->id, stream->name, size, stream->remoteFillPacketLevel, stream->remoteFillLevel, stream->writeSize);
        return 0;
    }

    return 1;
}

streamPacketDesc_t* getPacketFromStream(streamDesc_t* stream)
{
    streamPacketDesc_t* ret = NULL;
    if (stream->availablePackets)
    {
        ret = &stream->packets[stream->firstPacketUnused];
        stream->availablePackets--;
        CIRCULAR_INCREMENT(stream->firstPacketUnused,
                           XLINK_MAX_PACKETS_PER_STREAM);
        stream->blockedPackets++;
    }
    return ret;
}

// TODO add multithread access to the same stream; not currently supported
// due to issues like the order of get/move and release of packets
streamPacketDesc_t* movePacketFromStream(streamDesc_t* stream)
{
    streamPacketDesc_t *ret = NULL;
    if (stream->availablePackets)
    {
        ret = malloc(sizeof(streamPacketDesc_t));
        if (!ret)
        {
            mvLog(MVLOG_FATAL, "out of memory to move packet from stream\n");
            return NULL;
        }
        ret->data = NULL;
        ret->length = 0;
	ret->fd = -1;

        // copy fields of first unused packet
        *ret = stream->packets[stream->firstPacketUnused];

        // mark packet to no longer own data; keep length for later ack's
        stream->packets[stream->firstPacketUnused].data = NULL;

        // update circular buffer indices
        stream->availablePackets--;
        CIRCULAR_INCREMENT(stream->firstPacketUnused,
                           XLINK_MAX_PACKETS_PER_STREAM);
        stream->blockedPackets++;
    }
    return ret;
}

int releasePacketFromStream(streamDesc_t* stream, uint32_t* releasedSize)
{
    streamPacketDesc_t* currPack = &stream->packets[stream->firstPacket];
    if(stream->blockedPackets == 0){
        mvLog(MVLOG_ERROR,"There is no packet to release\n");
        return 0; // ignore this, although this is a big problem on application side
    }

    stream->localFillLevel -= currPack->length;
    mvLog(MVLOG_DEBUG, "S%d: Got release of %ld , current local fill level is %ld out of %ld %ld\n",
          stream->id, currPack->length, stream->localFillLevel, stream->readSize, stream->writeSize);

    XLinkPlatformDeallocateData(currPack->data,
                                ALIGN_UP_INT32((int32_t) currPack->length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);

    CIRCULAR_INCREMENT(stream->firstPacket, XLINK_MAX_PACKETS_PER_STREAM);
    stream->blockedPackets--;
    if (releasedSize) {
        *releasedSize = currPack->length;
    }
    return 0;
}

int releaseSpecificPacketFromStream(streamDesc_t* stream, uint32_t* releasedSize, uint8_t* data) {
    if (stream->blockedPackets == 0) {
        mvLog(MVLOG_ERROR,"There is no packet to release\n");
        return 0; // ignore this, although this is a big problem on application side
    }

    uint32_t packetId = stream->firstPacket;
    uint32_t found = 0;
    do {
        if (stream->packets[packetId].data == data) {
            found = 1;
            break;
        }
        CIRCULAR_INCREMENT(packetId, XLINK_MAX_PACKETS_PER_STREAM);
    } while (packetId != stream->firstPacketUnused);
    ASSERT_XLINK(found);

    streamPacketDesc_t* currPack = &stream->packets[packetId];
    if (currPack->length == 0) {
        mvLog(MVLOG_ERROR, "Packet with ID %d is empty\n", packetId);
    }

    stream->localFillLevel -= currPack->length;

  mvLog(MVLOG_DEBUG, "S%d: Got release of %ld , current local fill level is %ld out of %ld %ld\n",
          stream->id, currPack->length, stream->localFillLevel, stream->readSize, stream->writeSize);
    XLinkPlatformDeallocateData(currPack->data,
                                ALIGN_UP_INT32((int32_t) currPack->length, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
    stream->blockedPackets--;
    if (releasedSize) {
        *releasedSize = currPack->length;
    }

    if (packetId != stream->firstPacket) {
        uint32_t currIndex = packetId;
        uint32_t nextIndex = currIndex;
        CIRCULAR_INCREMENT(nextIndex, XLINK_MAX_PACKETS_PER_STREAM);
        while (currIndex != stream->firstPacketFree) {
            stream->packets[currIndex] = stream->packets[nextIndex];
            currIndex = nextIndex;
            CIRCULAR_INCREMENT(nextIndex, XLINK_MAX_PACKETS_PER_STREAM);
        }
        CIRCULAR_DECREMENT(stream->firstPacketUnused, (XLINK_MAX_PACKETS_PER_STREAM - 1));
        CIRCULAR_DECREMENT(stream->firstPacketFree, (XLINK_MAX_PACKETS_PER_STREAM - 1));

    } else {
        CIRCULAR_INCREMENT(stream->firstPacket, XLINK_MAX_PACKETS_PER_STREAM);
    }

    return 0;
}

int addNewPacketToStream(streamDesc_t* stream, void* buffer, uint32_t size, long fd, XLinkTimespec trsend, XLinkTimespec treceive) {
    if (stream->availablePackets + stream->blockedPackets < XLINK_MAX_PACKETS_PER_STREAM)
    {
        stream->packets[stream->firstPacketFree].data = buffer;
        stream->packets[stream->firstPacketFree].length = size;
        stream->packets[stream->firstPacketFree].fd = fd;
        stream->packets[stream->firstPacketFree].tRemoteSent = trsend;
        stream->packets[stream->firstPacketFree].tReceived = treceive;
        CIRCULAR_INCREMENT(stream->firstPacketFree, XLINK_MAX_PACKETS_PER_STREAM);
        stream->availablePackets++;
        return 0;
    }
    return -1;
}

int handleIncomingEvent(xLinkEvent_t* event, XLinkTimespec treceive) {
    //this function will be dependent whether this is a client or a Remote
    //specific actions to this peer
    mvLog(MVLOG_DEBUG, "%s, size %u, streamId %u.\n", TypeToStr(event->header.type), event->header.size, event->header.streamId);

    ASSERT_XLINK(event->header.type >= XLINK_WRITE_REQ
               && event->header.type != XLINK_REQUEST_LAST
               && event->header.type < XLINK_RESP_LAST);

    // Then read the data buffer, which is contained only in the XLINK_WRITE_REQ event
    if(event->header.type != XLINK_WRITE_REQ && event->header.type != XLINK_WRITE_FD_REQ) {
        return 0;
    }

    int rc = -1;
    streamDesc_t* stream = getStreamById(event->deviceHandle.xLinkFD, event->header.streamId);
    ASSERT_XLINK(stream);

    stream->localFillLevel += event->header.size;
    mvLog(MVLOG_DEBUG,"S%u: Got write of %u, current local fill level is %u out of %u %u\n",
          event->header.streamId, event->header.size, stream->localFillLevel, stream->readSize, stream->writeSize);

    void* buffer = XLinkPlatformAllocateData(ALIGN_UP(event->header.size, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
    XLINK_OUT_WITH_LOG_IF(buffer == NULL,
        mvLog(MVLOG_FATAL,"out of memory to receive data of size = %zu\n", event->header.size));

    long fd = -1;
    const int sc = XLinkPlatformRead(&event->deviceHandle, buffer, event->header.size, &fd);
    XLINK_OUT_WITH_LOG_IF(sc < 0, mvLog(MVLOG_ERROR,"%s() Read failed %d\n", __func__, sc));

    event->data = buffer;
    uint64_t tsec = event->header.tsecLsb | ((uint64_t)event->header.tsecMsb << 32);
    XLINK_OUT_WITH_LOG_IF(addNewPacketToStream(stream, buffer, event->header.size, fd, (XLinkTimespec){tsec, event->header.tnsec}, treceive),
        mvLog(MVLOG_WARN,"No more place in stream. release packet\n"));
    rc = 0;

XLINK_OUT:
    releaseStream(stream);

    if(rc != 0) {
        if(buffer != NULL) {
            XLinkPlatformDeallocateData(buffer,
                ALIGN_UP(event->header.size, __CACHE_LINE_SIZE), __CACHE_LINE_SIZE);
        }
        XLINK_EVENT_NOT_ACKNOWLEDGE(event);
    }

    return rc;
}

// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------
