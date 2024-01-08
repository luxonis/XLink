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

#include <stdlib.h>
#include <cstring>

struct fdPair {
	int usbFdRead;
	int usbFdWrite;
};

int usbEpInitialize() {
    return 0;
}

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
    return X_LINK_ERROR;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
    fdPair *pair = new fdPair;

    pair->usbFdRead = -1;
    pair->usbFdWrite = -1;

    if(devPathRead != NULL)
    {
	pair->usbFdRead = open(devPathRead, O_RDONLY);
    }

    if(devPathWrite != NULL)
    {
	pair->usbFdWrite = open(devPathWrite, O_WRONLY);
    }

    *fd = createPlatformDeviceFdKey((void*)pair);

    return 0;
}


int usbEpPlatformClose(void *fdKey)
{
    fdPair *pair;
    getPlatformDeviceFdFromKey(fdKey, (void**)&pair);

    int error;

    if (pair->usbFdRead > -1) {
	close(pair->usbFdRead);
	pair->usbFdRead = -1;
    }

    if (pair->usbFdWrite > -1) {
	close(pair->usbFdWrite);
	pair->usbFdWrite = -1;
    }

    destroyPlatformDeviceFdKey(fdKey);
    delete pair;

    return EXIT_SUCCESS;
}

int usbEpPlatformRead(void *fdKey, void *data, int size)
{
    fdPair *pair;
    getPlatformDeviceFdFromKey(fdKey, (void**)&pair);

    int rc = 0;

    if(pair->usbFdRead < 0)
    {
	return -1;
    }

    rc = read(pair->usbFdRead, data, size);

    return rc;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    fdPair *pair;
    getPlatformDeviceFdFromKey(fdKey, (void**)&pair);

    int rc = 0;

    if(pair->usbFdWrite < 0)
    {
	return -1;
    }

    rc = write(pair->usbFdWrite, data, size);
    return rc;
}

#else
int usbEpInitialize() {
    return X_LINK_ERROR;
}

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
    return X_LINK_ERROR;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
    return X_LINK_ERROR;
}


int usbEpPlatformClose(void *fdKey)
{
    return X_LINK_ERROR;
}

int usbEpPlatformRead(void *fdKey, void *data, int size)
{
    return X_LINK_ERROR;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    return X_LINK_ERROR;
}

#endif
