// project
#include <cstdlib>
#define MVLOG_UNIT_NAME xLinkUsb

#include "XLink/XLinkLog.h"
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "usb_host_ep.h"
#include "../PlatformDeviceFd.h"

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <libusb-1.0/libusb.h>

#include <cstring>

/* Vendor ID */
#define VENDOR_ID 0x03e7

/* Product ID */
#define PRODUCT_ID 0x1234

/* Interface number for ffs.xlink */
#define INTERFACE_XLINK 2

/* Base ndpoint address used for output */
#define ENDPOINT_OUT_BASE 0x01

/* Base endpoint address used for input */
#define ENDPOINT_IN_BASE 0x81

/* Transfer timeout */
#define TIMEOUT 2000

static int usbFdRead, usbFdWrite;
static bool isServer;

static libusb_context *ctx = NULL;
static libusb_device_handle *dev_handle = NULL;

int usbEpInitialize() {
    int error;

    /* Initialize libusb */
    libusb_init(&ctx);

    return 0;
}

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
    int error;
    isServer = false;

    /* Get our device */
    dev_handle = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
    if (dev_handle == NULL) {
	libusb_exit(ctx);

	error = LIBUSB_ERROR_NO_DEVICE;
	return error;
    }

    /* Not strictly necessary, but it is better to use it,
     * as we're using kernel modules together with our interfaces
     */
    error  = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (error != LIBUSB_SUCCESS) {
	libusb_exit(ctx);

	return error;
    }

    /* Now we claim our ffs interfaces */
    error = libusb_claim_interface(dev_handle, INTERFACE_XLINK);
    if (error != LIBUSB_SUCCESS) {
	libusb_exit(ctx);

	return error;
    }

    /* We get the first EP_OUT and EP_IN for our interfaces 
     * In the way we initialized our usb-gadget on our device
     * We know ncm is claiming the first 2 interfaces
     */
    usbFdWrite = ENDPOINT_OUT_BASE + 1; /* +1 because NCM claims 1 OUT endpoint */
    usbFdRead = ENDPOINT_IN_BASE + 2; /* +2 because NCM claims 2 IN endpoints */

    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);

    return 0;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
    isServer = true;

    char outPath[256];
    strcpy(outPath, devPathWrite);
    strcat(outPath, "/ep1");

    char inPath[256];
    strcpy(inPath, devPathWrite);
    strcat(inPath, "/ep2");

    int outfd = open(outPath, O_WRONLY);
    int infd = open(inPath, O_RDONLY);

    if(outfd < 0 || infd < 0) {
	return -1;
    }

    usbFdRead = infd;
    usbFdWrite = outfd;
    
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);

    return 0;
}


int usbEpPlatformClose(void *fdKey)
{
    int error;

    if (isServer) {
	if (usbFdRead != -1){
	    close(usbFdRead);
	    usbFdRead = -1;
	}

	if (usbFdWrite != -1){
	    close(usbFdWrite);
	    usbFdWrite = -1;
	}
    } else {
	error = libusb_release_interface(dev_handle, INTERFACE_XLINK);
	if (error != LIBUSB_SUCCESS) {
	    libusb_exit(ctx);

	    return error;
	}

	/* Release the device and exit */
	libusb_close(dev_handle);
    }

    libusb_exit(ctx);

    error = EXIT_SUCCESS;

    return EXIT_SUCCESS;
}

int usbEpPlatformRead(void *fdKey, void *data, int size)
{
    int rc = 0;

    if (isServer) {
	if(usbFdRead < 0)
	{
	    return -1;
	}

	rc = read(usbFdRead, data, size);
    } else {
	rc = libusb_bulk_transfer(dev_handle, usbFdRead, (unsigned char*)data, size, &rc, TIMEOUT);
    }

    return rc;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    int rc = 0;

    if (isServer) {
	if(usbFdWrite < 0)
	{
	    return -1;
	}

	rc = write(usbFdWrite, data, size);

    } else {
	rc = libusb_bulk_transfer(dev_handle, usbFdWrite, (unsigned char*)data, size, &rc, TIMEOUT);
    }

    return rc;
}


