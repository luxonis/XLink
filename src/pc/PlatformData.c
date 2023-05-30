// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "XLinkPlatform.h"
#include "XLinkPlatformErrorUtils.h"
#include "XLinkStringUtils.h"
#include "usb_host.h"
#include "pcie_host.h"
#include "tcpip_host.h"
#include "inttypes.h"

#define MVLOG_UNIT_NAME PlatformData
#include "XLinkLog.h"


// ------------------------------------
// Wrappers declaration. Begin.
// ------------------------------------

// ------------------------------------
// Wrappers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------
int XLinkPlatformEventSend(xLinkEvent_t *event) {
    // Enable specialization. If Linux & TCP_IP
#ifdef __linux__
    bool is_linux = true;
#else
    bool is_linux = false;
#endif

    if(is_linux && event->deviceHandle.protocol == X_LINK_TCP_IP) {

        #ifdef __linux__
        int tcpipPlatformWriteMulti(xLinkEvent_t* event)
        #endif
        return tcpipPlatformWriteMulti(event);

    } else {

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
        }

        return 0;

    }

    return 0;
}

int XLinkPlatformWrite(xLinkDeviceHandle_t *deviceHandle, void *data, int size)
{
    if(!XLinkIsProtocolInitialized(deviceHandle->protocol)) {
        return X_LINK_PLATFORM_DRIVER_NOT_LOADED+deviceHandle->protocol;
    }

    switch (deviceHandle->protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return usbPlatformWrite(deviceHandle->xLinkFD, data, size);

        case X_LINK_PCIE:
            return pciePlatformWrite(deviceHandle->xLinkFD, data, size);

        case X_LINK_TCP_IP:
            return tcpipPlatformWrite(deviceHandle->xLinkFD, data, size);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

int XLinkPlatformRead(xLinkDeviceHandle_t *deviceHandle, void *data, int size)
{
    if(!XLinkIsProtocolInitialized(deviceHandle->protocol)) {
        return X_LINK_PLATFORM_DRIVER_NOT_LOADED+deviceHandle->protocol;
    }

    switch (deviceHandle->protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return usbPlatformRead(deviceHandle->xLinkFD, data, size);

        case X_LINK_PCIE:
            return pciePlatformRead(deviceHandle->xLinkFD, data, size);

        case X_LINK_TCP_IP:
            return tcpipPlatformRead(deviceHandle->xLinkFD, data, size);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

void* XLinkPlatformAllocateData(uint32_t size, uint32_t alignment)
{
    void* ret = NULL;
#if (defined(_WIN32) || defined(_WIN64) )
    ret = _aligned_malloc(size, alignment);
#else
    if (posix_memalign(&ret, alignment, size) != 0) {
        perror("memalign failed");
    }
#endif
    return ret;
}

void XLinkPlatformDeallocateData(void *ptr, uint32_t size, uint32_t alignment)
{
    if (!ptr)
        return;
#if (defined(_WIN32) || defined(_WIN64) )
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

// ------------------------------------
// XLinkPlatform API implementation. End.
// ------------------------------------

