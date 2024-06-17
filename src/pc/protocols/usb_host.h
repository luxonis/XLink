// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "XLink/XLink.h"
#include "XLink/XLinkPlatform.h"

#define AUTO_VID                    0
#define AUTO_PID                    0
#define AUTO_UNBOOTED_PID           -1

#define DEFAULT_OPENVID             0x03E7
#ifdef ALTERNATE_PID
    #define DEFAULT_OPENPID             0xf63c      // Once opened in VSC mode, VID/PID change
#else
    #define DEFAULT_OPENPID             0xf63b     // Once opened in VSC mode, VID/PID change
#endif
#define DEFAULT_UNBOOTVID           0x03E7
#define DEFAULT_UNBOOTPID_2485      0x2485
#define DEFAULT_UNBOOTPID_2150      0x2150
#define DEFAULT_BOOTLOADER_PID      0xf63c
#define DEFAULT_FLASH_BOOTED_PID    0xf63d
#define DEFAULT_CHUNKSZ             1024*1024


typedef enum usbBootError {
    USB_BOOT_SUCCESS = 0,
    USB_BOOT_ERROR,
    USB_BOOT_DEVICE_NOT_FOUND,
    USB_BOOT_TIMEOUT
} usbBootError_t;

#ifdef XLINK_ENABLE_LIBUSB

xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices);
int usbInitialize(void* options);
int usbInitialize_customdir(void** hContext);

int usb_boot(const char *addr, const void *mvcmd, unsigned size);
int get_pid_by_name(const char* name);

xLinkPlatformErrorCode_t usbLinkBootBootloader(const char* path);
int usbPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd);
int usbPlatformClose(void *fd);
int usbPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length);

int usbPlatformRead(void *fd, void *data, int size);
int usbPlatformWrite(void *fd, void *data, int size);

#else

// Error out on these function calls
static inline int usbInitialize(void* options) { return -1; }
static inline xLinkPlatformErrorCode_t usbLinkBootBootloader(const char* path) { return X_LINK_PLATFORM_USB_DRIVER_NOT_LOADED; }

static inline int usbPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd) { return -1; }
static inline int usbPlatformClose(void *fd) { return -1; }
static inline int usbPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length) { return -1; }

static inline int usbPlatformRead(void *fd, void *data, int size) { return -1; }
static inline int usbPlatformWrite(void *fd, void *data, int size) { return -1; }

static inline xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) {
                                                        return X_LINK_PLATFORM_USB_DRIVER_NOT_LOADED;
                                                     }

#endif


#ifdef __cplusplus
}
#endif
