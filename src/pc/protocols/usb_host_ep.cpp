// project
#include <cstdlib>
#define MVLOG_UNIT_NAME xLinkUsb

#include "XLink/XLinkLog.h"
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "usb_host_ep.h"
#include "../PlatformDeviceFd.h"

#if not defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <cstring>

static int usbFdRead, usbFdWrite;

int usbEpInitialize() {
    return 0;
}

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
    return X_LINK_ERROR;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(_WIN32)
    return X_LINK_ERROR;
#else
    char outPath[256];
    strcpy(outPath, devPathWrite);
    strcat(outPath, "/ep1");

    char inPath[256];
    strcpy(inPath, devPathWrite);
    strcat(inPath, "/ep2");

    int outfd = open(outPath, O_RDWR);
    int infd = open(inPath, O_RDWR);

    if(outfd < 0 || infd < 0) {
	return -1;
    }

    usbFdRead = infd;
    usbFdWrite = outfd;

    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);

    return 0;
#endif
}


int usbEpPlatformClose(void *fdKey)
{
    int error;

#if defined(_WIN32)
    return X_LINK_ERROR;
#else
    if (usbFdRead != -1){
	close(usbFdRead);
	usbFdRead = -1;
    }

    if (usbFdWrite != -1){
	close(usbFdWrite);
	usbFdWrite = -1;
    }
#endif

    return EXIT_SUCCESS;
}

int usbEpPlatformRead(void *fdKey, void *data, int size)
{
    int rc = 0;

    if(usbFdRead < 0)
    {
	return -1;
    }
#if defined(_WIN32)
    return X_LINK_ERROR;
#else
    rc = read(usbFdRead, data, size);

#endif
    return rc;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    int rc = 0;

    if(usbFdWrite < 0)
    {
	return -1;
    }
#if defined(_WIN32)
    return X_LINK_ERROR;
#else
    rc = write(usbFdWrite, data, size);

#endif
    return rc;
}


