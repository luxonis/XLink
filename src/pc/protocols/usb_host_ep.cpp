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

#include <libusb-1.0/libusb.h>

#include <cstring>

/* Vendor ID */
#define VENDOR_ID 0x03e7

/* Product ID */
#define PRODUCT_ID 0x1234

/* Interface number for ffs.xlink */
#define INTERFACE_XLINK 0

/* Base ndpoint address used for output */
#define ENDPOINT_OUT_BASE 0x01

/* Base endpoint address used for input */
#define ENDPOINT_IN_BASE 0x81

/* Transfer timeout */
#define TIMEOUT 200

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
    usbFdWrite = ENDPOINT_OUT_BASE;
    usbFdRead = ENDPOINT_IN_BASE;

    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);

    return 0;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(_WIN32)
	return X_LINK_ERROR;
#else
    isServer = true;

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

    if (isServer) {
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
	    printf("Server read requested: %d\n", size);
	    if(usbFdRead < 0)
    	{
	        return -1;
	    }
#if defined(_WIN32)
    	return X_LINK_ERROR;
#else
	    rc = read(usbFdRead, data, size);
    
	    printf("Amount read data: %d\n", rc);
#endif
    } else {
	    int amount;
	    printf("Client read requested: %d\n", size);
	    rc = libusb_bulk_transfer(dev_handle, usbFdRead, (unsigned char*)data, size, &amount, TIMEOUT);

	    printf("Amount read data: %d\n", amount);
    }

    printf("Read return code: %d.\n", rc);

	if(rc < 0) return rc;
    return 0;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    int rc = 0;

    if (isServer) {
	    printf("Server write requested: %d\n", size);
	    if(usbFdWrite < 0)
	    {
	        return -1;
    	}
#if defined(_WIN32)
    	return X_LINK_ERROR;
#else
	    rc = write(usbFdWrite, data, size);
    
	    printf("Amount written data: %d\n", rc);
#endif
    } else {
	    int amount;
	    printf("Client write requested: %d\n", size);
    	rc = libusb_bulk_transfer(dev_handle, usbFdWrite, (unsigned char*)data, size, &amount, TIMEOUT);
	    
	printf("Amount written data: %d\n", amount);
    }
	    
printf("Write return code: %d.\n", rc);

	if(rc < 0) return rc;
    return 0;
}


