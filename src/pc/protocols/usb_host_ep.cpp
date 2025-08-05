// project
#include <cstdlib>
#define MVLOG_UNIT_NAME xLinkUsb

#include "XLink/XLinkLog.h"
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "usb_host_ep.h"
#include "../PlatformDeviceFd.h"
#include "usb_mx_id.h"
#include "usb_host.h"

// std
#include <mutex>
#include <cstring>
#include <string>

#if defined(__unix__)
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#endif

#include <libusb-1.0/libusb.h>

/* Vendor ID */
#define VENDOR_ID 0x05c6
#define PRODUCT_ID 0x4321

/* Interface number for ffs.gate */
#define INTERFACE_GATE 0

/* Interface number for ffs.xlink */
#define INTERFACE_XLINK 1
#define INTERFACE_XLINK_NAME "Luxonis Communication Interface"

/* Base ndpoint address used for output */
#define ENDPOINT_OUT_BASE 0x01

/* Base endpoint address used for input */
#define ENDPOINT_IN_BASE 0x81

/* The endpoint structure is the following
 * - Gate: first endpoint
 * - XLink: second endpoint
 * - ADB: third endpoint
 * - etc
 */
#define ENDPOINT_OUT_OFFSET 1
#define ENDPOINT_IN_OFFSET 1

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
    dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
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
        libusb_close(dev_handle);
	libusb_exit(ctx);

	return error;
    }
    
    libusb_device* dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor* config;
    error = libusb_get_active_config_descriptor(dev, &config);
    if (error != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
        return error;
    }

    // Try claiming interface 0 to test if it's in use
    bool found = false;
    bool foundGate = false;
    unsigned char name_buf[256];
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
	for (int j = 0; j < config->interface[i].num_altsetting; j++) {
        if(config->interface[i].altsetting[j].iInterface > 0) { 
	    int r = libusb_get_string_descriptor_ascii(dev_handle,
				config->interface[i].altsetting[j].iInterface,
				name_buf,
				sizeof(name_buf));

	    if (r > 0) {
	        name_buf[r] = '\0';
    	        if (strcmp((char*)name_buf, INTERFACE_XLINK_NAME) == 0) {
		    if(!foundGate) {
			foundGate = true;
		    } else {
	            	found = true;
	            	break;
		    }
	        }
	    }
	}
	}
    }

    libusb_free_config_descriptor(config);

    if (!found) {
        libusb_close(dev_handle);
        libusb_exit(ctx);

	return LIBUSB_ERROR_NO_DEVICE;
    }    

    /* Now we claim our ffs interfaces */
    error = libusb_claim_interface(dev_handle, INTERFACE_XLINK);
    if (error != LIBUSB_SUCCESS) {
	libusb_exit(ctx);

	return error;
    }

    /* We get the first EP_OUT and EP_IN for our interfaces 
     * In the way we initialized our usb-gadget on our device
     */
    usbFdWrite = ENDPOINT_OUT_BASE + ENDPOINT_OUT_OFFSET;
    usbFdRead = ENDPOINT_IN_BASE + ENDPOINT_IN_OFFSET;

    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);

    return 0;
}

int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
    isServer = true;

#if defined(__unix__)
    int outfd = open("/dev/usb-ffs/xlink/ep1", O_WRONLY);
    int infd = open("/dev/usb-ffs/xlink/ep2", O_RDONLY);

    if(outfd < 0 || infd < 0) {
	return -1;
    }

    usbFdRead = infd;
    usbFdWrite = outfd;
 
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);
#endif

    return 0;
}


int usbEpPlatformClose(void *fdKey)
{
    int error;

    if (isServer) {
#if defined(__unix__)
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
#if defined(__unix__)
	if(usbFdRead < 0)
	{
	    return -1;
	}

	rc = read(usbFdRead, data, size);
#endif
    } else {
	rc = libusb_bulk_transfer(dev_handle, usbFdRead, (unsigned char*)data, size, &rc, TIMEOUT);
    }

    return rc;
}

int usbEpPlatformWrite(void *fdKey, void *data, int size)
{
    int rc = 0;

    if (isServer) {
#if defined(__unix__)
	if(usbFdWrite < 0)
	{
	    return -1;
	}

	rc = write(usbFdWrite, data, size);
#endif
    } else {
	rc = libusb_bulk_transfer(dev_handle, usbFdWrite, (unsigned char*)data, size, &rc, TIMEOUT);
    }

    return rc;
}

int usbepGetDevices(const deviceDesc_t in_deviceRequirements,
                                                    deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                    unsigned int *out_amountOfFoundDevices) {
    int error = 0;
    libusb_device_handle *dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (dev_handle == NULL) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    error = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (error != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        return error;
    }

    // Check if the interface exists
    libusb_device* dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor* config;
    error = libusb_get_active_config_descriptor(dev, &config);
    if (error != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        return error;
    }

    // Look for INTERFACE_XLINK
    bool found = false;
    unsigned char name_buf[256];
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        if (config->interface[i].altsetting[0].bInterfaceNumber == INTERFACE_XLINK) {
	    if(config->interface[i].altsetting[0].iInterface > 0) {
                int r = libusb_get_string_descriptor_ascii(dev_handle,
				config->interface[i].altsetting[0].iInterface,
				name_buf,
				sizeof(name_buf));

		if (r > 0) {
		    name_buf[r] = '\0';

		    if (strcmp((char*)name_buf, INTERFACE_XLINK_NAME) == 0) {
		        found = true;
	                break;
		    }
		}
	    }
        }
    }

    libusb_free_config_descriptor(config);

    if (!found) {
        libusb_close(dev_handle);

	*out_amountOfFoundDevices = 0;

	return X_LINK_PLATFORM_SUCCESS;
    }           

    // Get device descriptor
    struct libusb_device_descriptor desc;
    int r = libusb_get_device_descriptor(dev, &desc);
    if (r < 0) {
	return X_LINK_PLATFORM_ERROR;
    }

    // Get device mxid
    char mxId[32] = {'\0'};
        
    r = libusb_get_string_descriptor_ascii(dev_handle, desc.iSerialNumber, ((uint8_t*) mxId), 32);

    int numDevicesFound = 0;
    // Everything passed, fillout details of found device
    out_foundDevices[numDevicesFound].status = X_LINK_SUCCESS;
    out_foundDevices[numDevicesFound].platform = X_LINK_RVC4;
    out_foundDevices[numDevicesFound].protocol = X_LINK_USB_EP;
    out_foundDevices[numDevicesFound].state = X_LINK_GATE;
    memset(out_foundDevices[numDevicesFound].name, 0, sizeof(out_foundDevices[numDevicesFound].name));
    strcpy(out_foundDevices[numDevicesFound].name, "USB EP");
    memset(out_foundDevices[numDevicesFound].mxid, 0, sizeof(out_foundDevices[numDevicesFound].mxid));
    strcpy(out_foundDevices[numDevicesFound].mxid, mxId);
    numDevicesFound++;

    // Write the number of found devices
    *out_amountOfFoundDevices = numDevicesFound;

    libusb_close(dev_handle);

    return X_LINK_PLATFORM_SUCCESS;
}


int usbEpPlatformGateRead(void *data, int size)
{
    int rc = 0;

    /* Get our device */
    libusb_device_handle *dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (dev_handle == NULL) {
	libusb_exit(ctx);

	rc = LIBUSB_ERROR_NO_DEVICE;
	return rc;
    }
    
    /* Not strictly necessary, but it is better to use it,
     * as we're using kernel modules together with our interfaces
     */
    rc  = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (rc != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
	libusb_exit(ctx);

	return rc;
    }
    
    libusb_device* dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor* config;
    rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
        return rc;
    }

    // Try claiming interface 0 to test if it's in use
    bool found = false;
    unsigned char name_buf[256];
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
	for (int j = 0; j < config->interface[i].num_altsetting; j++) {
        if(config->interface[i].altsetting[j].iInterface > 0) { 
	    int r = libusb_get_string_descriptor_ascii(dev_handle,
				config->interface[i].altsetting[j].iInterface,
				name_buf,
				sizeof(name_buf));

	    if (r > 0) {
	        name_buf[r] = '\0';
    	        if (strcmp((char*)name_buf, INTERFACE_XLINK_NAME) == 0) {
	            found = true;
	        }
	    }
	}
	}
    }

    libusb_free_config_descriptor(config);

    if (!found) {
        libusb_close(dev_handle);
        libusb_exit(ctx);

	return LIBUSB_ERROR_NO_DEVICE;
    }    

    /* Now we claim our ffs interfaces */
    rc = libusb_claim_interface(dev_handle, INTERFACE_GATE);
    if (rc != LIBUSB_SUCCESS) {
	libusb_exit(ctx);

	return rc;
    }

    rc = libusb_bulk_transfer(dev_handle, ENDPOINT_IN_BASE, (unsigned char*)data, size, &rc, TIMEOUT);
    
    libusb_close(dev_handle);

    return rc;
}

int usbEpPlatformGateWrite(void *data, int size)
{
    int rc = 0;

    /* Get our device */
    libusb_device_handle *dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (dev_handle == NULL) {
	libusb_exit(ctx);

	rc = LIBUSB_ERROR_NO_DEVICE;
	return rc;
    }
    
    /* Not strictly necessary, but it is better to use it,
     * as we're using kernel modules together with our interfaces
     */
    rc  = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (rc != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
	libusb_exit(ctx);

	return rc;
    }
    
    libusb_device* dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor* config;
    rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
        return rc;
    }

    // Try claiming interface 0 to test if it's in use
    bool found = false;
    unsigned char name_buf[256];
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
	for (int j = 0; j < config->interface[i].num_altsetting; j++) {
        if(config->interface[i].altsetting[j].iInterface > 0) { 
	    int r = libusb_get_string_descriptor_ascii(dev_handle,
				config->interface[i].altsetting[j].iInterface,
				name_buf,
				sizeof(name_buf));

	    if (r > 0) {
	        name_buf[r] = '\0';
    	        if (strcmp((char*)name_buf, INTERFACE_XLINK_NAME) == 0) {
	            found = true;
	        }
	    }
	}
	}
    }

    libusb_free_config_descriptor(config);

    if (!found) {
        libusb_close(dev_handle);
        libusb_exit(ctx);

	return LIBUSB_ERROR_NO_DEVICE;
    }    

    /* Now we claim our ffs interfaces */
    rc = libusb_claim_interface(dev_handle, INTERFACE_GATE);
    if (rc != LIBUSB_SUCCESS) {
	libusb_exit(ctx);

	return rc;
    }

    rc = libusb_bulk_transfer(dev_handle, ENDPOINT_OUT_BASE, (unsigned char*)data, size, &rc, TIMEOUT);
    
    libusb_close(dev_handle);

    return rc;
}
