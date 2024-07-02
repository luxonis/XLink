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
#include "local_memshd.h"
#include "PlatformDeviceFd.h"
#include "inttypes.h"

#define MVLOG_UNIT_NAME PlatformData
#include "XLinkLog.h"

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
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
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

#ifdef USE_LINK_JTAG
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif  /*USE_LINK_JTAG*/

#ifndef USE_USB_VSC
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "usb_host.h"

extern int usbFdWrite;
extern int usbFdRead;
#endif  /*USE_USB_VSC*/

// ------------------------------------
// Wrappers declaration. Begin.
// ------------------------------------

static int pciePlatformRead(void *f, void *data, int size);
static int pciePlatformWrite(void *f, void *data, int size);

// ------------------------------------
// Wrappers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

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

#if defined(__unix__)
	case X_LINK_LOCAL_SHDMEM:
	    return shdmemPlatformWrite(deviceHandle->xLinkFD, data, size);
#endif

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

int XLinkPlatformWriteFd(xLinkDeviceHandle_t *deviceHandle, const long fd, void *data2, int size2)
{
    if(!XLinkIsProtocolInitialized(deviceHandle->protocol)) {
        return X_LINK_PLATFORM_DRIVER_NOT_LOADED+deviceHandle->protocol;
    }

    switch (deviceHandle->protocol) {
#if defined(__unix__)
	case X_LINK_LOCAL_SHDMEM:
	    return shdmemPlatformWriteFd(deviceHandle->xLinkFD, fd, data2, size2);

	case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
        case X_LINK_PCIE:
        case X_LINK_TCP_IP:
	    {
		if (fd <= 0) {
		    return X_LINK_ERROR;
		}

	        // Determine file size through fstat
		struct stat fileStats;
		fstat(fd, &fileStats);
		int size = fileStats.st_size;

		// mmap the fine in memory
		void *addr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
		if (addr == MAP_FAILED) {
		    mvLog(MVLOG_ERROR, "Failed to mmap file to stream it over\n");
		    return X_LINK_ERROR;
		}

		// Use the respective write function to copy and send the message
		int result = X_LINK_ERROR;
		switch(deviceHandle->protocol) {
		    case X_LINK_USB_VSC:
		    case X_LINK_USB_CDC:
			result = usbPlatformWrite(deviceHandle->xLinkFD, addr, size);
			break;
		    case X_LINK_PCIE:
			result = pciePlatformWrite(deviceHandle->xLinkFD, addr, size);
			break;
		    case X_LINK_TCP_IP:
			result = tcpipPlatformWrite(deviceHandle->xLinkFD, addr, size);
			break;
		    default:
			result = X_LINK_PLATFORM_INVALID_PARAMETERS;
			break;
		}

		// Unmap file
		munmap(addr, size);
	    
		return result;
	    }
#endif
        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

int XLinkPlatformRead(xLinkDeviceHandle_t *deviceHandle, void *data, int size, long *fd)
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
	
#if defined(__unix__)
	case X_LINK_LOCAL_SHDMEM:
	    return shdmemPlatformRead(deviceHandle->xLinkFD, data, size, fd);
#endif

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

        size_t chunk = (size_t)size < CHUNK_SIZE_BYTES ? (size_t)size : CHUNK_SIZE_BYTES;
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



// ------------------------------------
// Wrappers implementation. End.
// ------------------------------------
