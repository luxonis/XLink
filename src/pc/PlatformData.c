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
#include "usb_boot.h"
#include "pcie_host.h"
#include "tcpip_host.h"

#define MVLOG_UNIT_NAME PlatformData
#include "XLinkLog.h"

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include "win_usb.h"
#include "win_time.h"
#include "win_pthread.h"
#include <winsock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <libusb.h>
#include <signal.h>
#endif

#ifdef USE_LINK_JTAG
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif  /*USE_LINK_JTAG*/

#define USB_ENDPOINT_IN 0x81
#define USB_ENDPOINT_OUT 0x01

#ifndef USE_USB_VSC
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>

extern int usbFdWrite;
extern int usbFdRead;
#endif  /*USE_USB_VSC*/

#ifndef XLINK_USB_DATA_TIMEOUT
#define XLINK_USB_DATA_TIMEOUT 0
#endif

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------
#ifdef USE_USB_VSC
static int usb_write(libusb_device_handle *f, const void *data, size_t size);
static int usb_read(libusb_device_handle *f, void *data, size_t size);
#endif
// ------------------------------------
// Helpers declaration. End.
// ------------------------------------



// ------------------------------------
// Wrappers declaration. Begin.
// ------------------------------------

static int usbPlatformRead(void *fd, void *data, int size);
static int pciePlatformRead(void *f, void *data, int size);
static int tcpipPlatformRead(void *fd, void *data, int size);

static int usbPlatformWrite(void *fd, void *data, int size);
static int pciePlatformWrite(void *f, void *data, int size);
static int tcpipPlatformWrite(void *fd, void *data, int size);

// ------------------------------------
// Wrappers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

int XLinkPlatformWrite(xLinkDeviceHandle_t *deviceHandle, void *data, int size)
{
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



// ------------------------------------
// Wrappers implementation. Begin.
// ------------------------------------

int usbPlatformRead(void* fd, void* data, int size)
{
    int rc = 0;
#ifndef USE_USB_VSC
    int nread =  0;
#ifdef USE_LINK_JTAG
    while (nread < size){
        nread += read(usbFdWrite, &((char*)data)[nread], size - nread);
        printf("read %d %d\n", nread, size);
    }
#else
    if(usbFdRead < 0)
    {
        return -1;
    }

    while(nread < size)
    {
        int toRead = (PACKET_LENGTH && (size - nread > PACKET_LENGTH)) \
                        ? PACKET_LENGTH : size - nread;

        while(toRead > 0)
        {
            rc = read(usbFdRead, &((char*)data)[nread], toRead);
            if ( rc < 0)
            {
                return -2;
            }
            toRead -=rc;
            nread += rc;
        }
        unsigned char acknowledge = 0xEF;
        int wc = write(usbFdRead, &acknowledge, sizeof(acknowledge));
        if (wc != sizeof(acknowledge))
        {
            return -2;
        }
    }
#endif  /*USE_LINK_JTAG*/
#else
    rc = usb_read((libusb_device_handle *) fd, data, size);
#endif  /*USE_USB_VSC*/
    return rc;
}

int usbPlatformWrite(void *fd, void *data, int size)
{
    int rc = 0;
#ifndef USE_USB_VSC
    int byteCount = 0;
#ifdef USE_LINK_JTAG
    while (byteCount < size){
        byteCount += write(usbFdWrite, &((char*)data)[byteCount], size - byteCount);
        printf("write %d %d\n", byteCount, size);
    }
#else
    if(usbFdWrite < 0)
    {
        return -1;
    }
    while(byteCount < size)
    {
       int toWrite = (PACKET_LENGTH && (size - byteCount > PACKET_LENGTH)) \
                        ? PACKET_LENGTH:size - byteCount;
       int wc = write(usbFdWrite, ((char*)data) + byteCount, toWrite);

       if ( wc != toWrite)
       {
           return -2;
       }

       byteCount += toWrite;
       unsigned char acknowledge;
       int rc;
       rc = read(usbFdWrite, &acknowledge, sizeof(acknowledge));

       if ( rc < 0)
       {
           return -2;
       }

       if (acknowledge != 0xEF)
       {
           return -2;
       }
    }
#endif  /*USE_LINK_JTAG*/
#else
    rc = usb_write((libusb_device_handle *) fd, data, size);
#endif  /*USE_USB_VSC*/
    return rc;
}


#if (defined(_WIN32) || defined(_WIN64))
static int write_pending = 0;
static int read_pending = 0;
#endif

int pciePlatformWrite(void *f, void *data, int size)
{
#if (defined(_WIN32) || defined(_WIN64))
    #define CHUNK_SIZE_BYTES (5ULL * 1024ULL * 1024ULL)

    while (size)
    {
        write_pending = 1;

        size_t chunk = size < CHUNK_SIZE_BYTES ? size : CHUNK_SIZE_BYTES;
        int num_written = pcie_write(f, data, chunk);

        write_pending = 0;

        if (num_written == -EAGAIN)  {
            // Let read commands be submitted
            if (read_pending > 0) {
                usleep(1000);
            }
            continue;
        }

        if (num_written < 0) {
            return num_written;
        }

        data = ((char*) data) + num_written;
        /**
         * num_written is always not greater than size
         */
        size -= num_written;
    }

    return 0;
#undef CHUNK_SIZE_BYTES
#else       // Linux case
    int left = size;

    while (left > 0)
    {
        int bt = pcie_write(f, data, left);
        if (bt < 0)
            return bt;

        data = ((char *)data) + bt;
        left -= bt;
    }

    return 0;
#endif
}

int pciePlatformRead(void *f, void *data, int size)
{
#if (defined(_WIN32) || defined(_WIN64))
    while (size)
    {
        read_pending = 1;

        int num_read = pcie_read(f, data, size);

        read_pending = 0;

        if (num_read == -EAGAIN)  {
            // Let write commands be submitted
            if (write_pending > 0) {
                usleep(1000);
            }
            continue;
        }

        if(num_read < 0) {
            return num_read;
        }

        data = ((char *)data) + num_read;
        /**
         * num_read is always not greater than size
         */
        size -= num_read;
    }

    return 0;
#else       // Linux
    int left = size;

    while (left > 0)
    {
        int bt = pcie_read(f, data, left);
        if (bt < 0)
            return bt;

        data = ((char *)data) + bt;
        left -= bt;
    }

    return 0;
#endif
}

static int tcpipPlatformRead(void *fd, void *data, int size)
{
#if defined(USE_TCP_IP)
    int nread = 0;
    int rc = -1;

    while(nread < size)
    {
        // TMP TMP - leaky test
        TCPIP_SOCKET sock = *((TCPIP_SOCKET*)fd);
        // TCPIP_SOCKET sock = (TCPIP_SOCKET) fd;

        rc = recv((TCPIP_SOCKET)sock, &((char*)data)[nread], size - nread, 0);
        if(rc <= 0)
        {
            return -1;
        }
        else
        {
            nread += rc;
            rc = -1;
        }
    }
#endif
    return 0;
}

static int tcpipPlatformWrite(void *fd, void *data, int size)
{
#if defined(USE_TCP_IP)
    int byteCount = 0;
    int rc = -1;

    while(byteCount < size)
    {
        // Use send instead of write and ignore SIGPIPE
        //rc = write((intptr_t)fd, &((char*)data)[byteCount], size - byteCount);

        int flags = 0;
        #if defined(MSG_NOSIGNAL)
            // Use flag NOSIGNAL on send call
            flags = MSG_NOSIGNAL;
        #endif

        // TMP TMP - leaky test
        TCPIP_SOCKET sock = *((TCPIP_SOCKET*)fd);
        // TCPIP_SOCKET sock = (TCPIP_SOCKET) fd;

        rc = send(sock, &((char*)data)[byteCount], size - byteCount, flags);
        if(rc <= 0)
        {
            return -1;
        }
        else
        {
            byteCount += rc;
            rc = -1;
        }
    }
#endif
    return 0;
}

// ------------------------------------
// Wrappers implementation. End.
// ------------------------------------



// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------
#ifdef USE_USB_VSC
int usb_read(libusb_device_handle *f, void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
#if (defined(_WIN32) || defined(_WIN64))
        int rc = usb_bulk_read(f, USB_ENDPOINT_IN, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
#else
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_IN,(unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
#endif
        if(rc)
            return rc;
        data = ((char *)data) + bt;
        size -= bt;
    }
    return 0;
}

int usb_write(libusb_device_handle *f, const void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
#if (defined(_WIN32) || defined(_WIN64) )
        int rc = usb_bulk_write(f, USB_ENDPOINT_OUT, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
#else
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_OUT, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
#endif
        if(rc)
            return rc;
        data = (char *)data + bt;
        size -= bt;
    }
    return 0;
}
#endif
// ------------------------------------
// Helpers implementation. End.
// ------------------------------------
