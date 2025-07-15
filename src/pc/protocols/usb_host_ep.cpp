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
#include <string>
#include <cstring>

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <libusb-1.0/libusb.h>

/* Vendor ID */
#define VENDOR_ID 0x05c6

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

libusb_device_handle *findUnusedDevice() {
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) return NULL;

    libusb_device_handle *handle = NULL;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(dev, &desc) != 0)
            continue;
    
        if (desc.idVendor != VENDOR_ID)
            continue;
	
        if (libusb_open(dev, &handle) != 0)
            continue;
	
        if (handle) {
            break; /* Found available device */
        }
    }

    libusb_free_device_list(devs, 1);
    return handle;
}

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
    int error;
    isServer = false;

    /* Get our device */
    dev_handle = findUnusedDevice();
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

    int outfd = open("/dev/usb-ffs/xlink/ep1", O_WRONLY);
    int infd = open("/dev/usb-ffs/xlink/ep2", O_RDONLY);

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

int usbepGetDevices(const deviceDesc_t in_deviceRequirements,
                                                    deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                    unsigned int *out_amountOfFoundDevices) {
    int error = 0;
    libusb_device_handle* dev_handle = findUnusedDevice();
    if (dev_handle == NULL) {
        libusb_exit(ctx);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    error = libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (error != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
        return error;
    }

    // Check if the interface exists
    libusb_device* dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor* config;
    error = libusb_get_active_config_descriptor(dev, &config);
    if (error != LIBUSB_SUCCESS) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
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
        libusb_exit(ctx);

	*out_amountOfFoundDevices = 0;

	return X_LINK_PLATFORM_SUCCESS;
    }           

    int numDevicesFound = 0;
    // Everything passed, fillout details of found device
    out_foundDevices[numDevicesFound].status = X_LINK_SUCCESS;
    out_foundDevices[numDevicesFound].platform = X_LINK_MYRIAD_X;
    out_foundDevices[numDevicesFound].protocol = X_LINK_USB_VSC;
    out_foundDevices[numDevicesFound].state = X_LINK_BOOTED;
    memset(out_foundDevices[numDevicesFound].name, 0, sizeof(out_foundDevices[numDevicesFound].name));
    memset(out_foundDevices[numDevicesFound].mxid, 0, sizeof(out_foundDevices[numDevicesFound].mxid));
    numDevicesFound++;

    // Write the number of found devices
    *out_amountOfFoundDevices = numDevicesFound;

    libusb_close(dev_handle);
    libusb_exit(ctx);

    return X_LINK_PLATFORM_SUCCESS;
}


/// TODO - use this for device search.
xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) {

    // Also protects usb_mx_id_cache
    std::lock_guard<std::mutex> l(mutex);

    // No RVC3/4 devices on USB now, return 0
    if(in_deviceRequirements.platform == X_LINK_RVC3 || in_deviceRequirements.platform == X_LINK_RVC4){
        *out_amountOfFoundDevices = 0;
        return X_LINK_PLATFORM_SUCCESS;
    }


    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto numDevices = libusb_get_device_list(context, &devs);
    if(numDevices < 0) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(static_cast<int>(numDevices)));
        return X_LINK_PLATFORM_ERROR;
    }

    /// NOT NEEDED
    // // Initialize mx id cache
    // usb_mx_id_cache_init();

    // Loop over all usb devices, increase count only if myriad device
    int numDevicesFound = 0;
    for(ssize_t i = 0; i < numDevices; i++) {
        if(devs[i] == nullptr) continue;

        if(numDevicesFound >= sizeFoundDevices){
            break;
        }

        // Get device descriptor
        struct libusb_device_descriptor desc;
        auto res = libusb_get_device_descriptor(devs[i], &desc);
        if (res < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device descriptor: %s", xlink_libusb_strerror(res));
            continue;
        }

        /// TODO - modify to use updated VID/PID
        VidPid vidpid{desc.idVendor, desc.idProduct};

        if(vidPidToDeviceState.count(vidpid) > 0){
            // Device found

            // Device status
            XLinkError_t status = X_LINK_SUCCESS;

            // Get device state
            XLinkDeviceState_t state = vidPidToDeviceState.at(vidpid);
            // Check if compare with state
            if(in_deviceRequirements.state != X_LINK_ANY_STATE && state != in_deviceRequirements.state){
                // Current device doesn't match the "filter"
                continue;
            }

            // Get device name
            std::string devicePath = getLibusbDevicePath(devs[i]);
            // Check if compare with name, if name is only a hint, don't filter

            if(!in_deviceRequirements.nameHintOnly){
                std::string requiredName(in_deviceRequirements.name);
                if(requiredName.length() > 0 && requiredName != devicePath){
                    // Current device doesn't match the "filter"
                    continue;
                }
            }

            // Get device mxid
            std::string mxId;
        
            libusb_error rc = libusb_get_string_descriptor_ascii(handle, pDesc->iSerialNumber, ((uint8_t*) mxId), sizeof(mxId)));
                            
            switch (rc)
            {
            case LIBUSB_SUCCESS:
                status = X_LINK_SUCCESS;
                break;
            case LIBUSB_ERROR_ACCESS:
                status = X_LINK_INSUFFICIENT_PERMISSIONS;
                break;
            case LIBUSB_ERROR_BUSY:
                status = X_LINK_DEVICE_ALREADY_IN_USE;
                break;
            default:
                status = X_LINK_ERROR;
                break;
            }

            // compare with MxId
            std::string requiredMxId(in_deviceRequirements.mxid);
            if(requiredMxId.length() > 0 && requiredMxId != mxId){
                // Current device doesn't match the "filter"
                continue;
            }

            // TODO(themarpe) - check platform

            // Everything passed, fillout details of found device
            out_foundDevices[numDevicesFound].status = status;
            out_foundDevices[numDevicesFound].platform = X_LINK_MYRIAD_X;
            out_foundDevices[numDevicesFound].protocol = X_LINK_USB_VSC;
            out_foundDevices[numDevicesFound].state = state;
            memset(out_foundDevices[numDevicesFound].name, 0, sizeof(out_foundDevices[numDevicesFound].name));
            strncpy(out_foundDevices[numDevicesFound].name, devicePath.c_str(), sizeof(out_foundDevices[numDevicesFound].name));
            memset(out_foundDevices[numDevicesFound].mxid, 0, sizeof(out_foundDevices[numDevicesFound].mxid));
            strncpy(out_foundDevices[numDevicesFound].mxid, mxId.c_str(), sizeof(out_foundDevices[numDevicesFound].mxid));
            numDevicesFound++;

        }

    }

    // Free list of usb devices
    libusb_free_device_list(devs, 1);

    // Write the number of found devices
    *out_amountOfFoundDevices = numDevicesFound;

    return X_LINK_PLATFORM_SUCCESS;
}
