// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>

#include "XLinkPlatform.h"
#include "XLinkPlatformErrorUtils.h"
#include "usb_host.h"
#include "pcie_host.h"
#include "tcpip_host.h"
#include "XLinkStringUtils.h"


#define MVLOG_UNIT_NAME PlatformDeviceSearch
#include "XLinkLog.h"

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static int platformToPid(const XLinkPlatform_t platform, const XLinkDeviceState_t state);
static pciePlatformState_t xlinkDeviceStateToPciePlatformState(const XLinkDeviceState_t state);

static xLinkPlatformErrorCode_t parseUsbBootError(usbBootError_t rc);
static xLinkPlatformErrorCode_t parsePCIeHostError(pcieHostError_t rc);

xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices);
static xLinkPlatformErrorCode_t getPCIeDeviceName(int index,
                                                  XLinkDeviceState_t state,
                                                  const deviceDesc_t in_deviceRequirements,
                                                  deviceDesc_t* out_foundDevice);
static xLinkPlatformErrorCode_t getTcpIpDevices(const deviceDesc_t in_deviceRequirements,
                                                    deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                    unsigned int *out_amountOfFoundDevices);


// ------------------------------------
// Helpers declaration. End.
// ------------------------------------


// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

xLinkPlatformErrorCode_t XLinkPlatformFindDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, unsigned sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) {
    memset(out_foundDevices, sizeFoundDevices, sizeof(deviceDesc_t));
    xLinkPlatformErrorCode_t USB_rc;
    xLinkPlatformErrorCode_t PCIe_rc;
    xLinkPlatformErrorCode_t TCPIP_rc;
    unsigned numFoundDevices = 0;
    *out_amountOfFoundDevices = 0;

    switch (in_deviceRequirements.protocol){
        case XLINK_USB_CDC:
        case XLINK_USB_VSC:
            if(!XLinkIsProtocolInitialized(in_deviceRequirements.protocol)) {
                return XLINK_PLATFORM_DRIVER_NOT_LOADED+in_deviceRequirements.protocol;
            }
            // Check if protocol is initialized
            return getUSBDevices(in_deviceRequirements, out_foundDevices, sizeFoundDevices, out_amountOfFoundDevices);

        /* TODO(themarpe) - reenable PCIe
        case XLINK_PCIE:
            return getPCIeDeviceName(0, state, in_deviceRequirements, out_foundDevice);
        */

        case XLINK_TCP_IP:
            if(!XLinkIsProtocolInitialized(in_deviceRequirements.protocol)) {
                return XLINK_PLATFORM_DRIVER_NOT_LOADED+in_deviceRequirements.protocol;
            }
            return getTcpIpDevices(in_deviceRequirements, out_foundDevices, sizeFoundDevices, out_amountOfFoundDevices);

        case XLINK_ANY_PROTOCOL:

            // If USB protocol is initialized
            if(XLinkIsProtocolInitialized(XLINK_USB_VSC)) {
                // Find first correct USB Device
                numFoundDevices = 0;
                USB_rc = getUSBDevices(in_deviceRequirements, out_foundDevices, sizeFoundDevices, &numFoundDevices);
                *out_amountOfFoundDevices += numFoundDevices;
                out_foundDevices += numFoundDevices;
                // Found enough devices, return
                if (numFoundDevices >= sizeFoundDevices) {
                    return XLINK_PLATFORM_SUCCESS;
                } else {
                    sizeFoundDevices -= numFoundDevices;
                }
            }


            /* TODO(themarpe) - reenable PCIe
            if(XLinkIsProtocolInitialized(XLINK_PCIE)) {
                numFoundDevices = 0;
                PCIe_rc = getPCIeDeviceName(0, state, in_deviceRequirements, out_foundDevice);
                // Found enough devices, return
                out_foundDevices += numFoundDevices;
                if (numFoundDevices >= sizeFoundDevices) {
                    return XLINK_PLATFORM_SUCCESS;
                } else {
                    sizeFoundDevices -= numFoundDevices;
                }
                *out_amountOfFoundDevices += numFoundDevices;
            }
            */

            // Try find TCPIP device
            if(XLinkIsProtocolInitialized(XLINK_TCP_IP)) {
                numFoundDevices = 0;
                TCPIP_rc = getTcpIpDevices(in_deviceRequirements, out_foundDevices, sizeFoundDevices, &numFoundDevices);
                *out_amountOfFoundDevices += numFoundDevices;
                out_foundDevices += numFoundDevices;
                sizeFoundDevices -= numFoundDevices;
                // Found enough devices, return
                if (numFoundDevices >= sizeFoundDevices) {
                    return XLINK_PLATFORM_SUCCESS;
                } else {
                    sizeFoundDevices -= numFoundDevices;
                }
            }

            return XLINK_PLATFORM_SUCCESS;

        default:
            mvLog(MVLOG_WARN, "Unknown protocol");
            return XLINK_PLATFORM_INVALID_PARAMETERS;
    }

    return XLINK_PLATFORM_SUCCESS;
}


int XLinkPlatformIsDescriptionValid(const deviceDesc_t *in_deviceDesc, const XLinkDeviceState_t state) {
    if(!in_deviceDesc){
        return 0;
    }

    return 1;
}

char* XLinkPlatformErrorToStr(const xLinkPlatformErrorCode_t errorCode) {
    switch (errorCode) {
        case XLINK_PLATFORM_SUCCESS: return "XLINK_PLATFORM_SUCCESS";
        case XLINK_PLATFORM_DEVICE_NOT_FOUND: return "XLINK_PLATFORM_DEVICE_NOT_FOUND";
        case XLINK_PLATFORM_ERROR: return "XLINK_PLATFORM_ERROR";
        case XLINK_PLATFORM_TIMEOUT: return "XLINK_PLATFORM_TIMEOUT";
        case XLINK_PLATFORM_USB_DRIVER_NOT_LOADED: return "XLINK_PLATFORM_USB_DRIVER_NOT_LOADED";
        case XLINK_PLATFORM_TCP_IP_DRIVER_NOT_LOADED: return "XLINK_PLATFORM_TCP_IP_DRIVER_NOT_LOADED";
        case XLINK_PLATFORM_PCIE_DRIVER_NOT_LOADED: return "XLINK_PLATFORM_PCIE_DRIVER_NOT_LOADED";
        case XLINK_PLATFORM_INVALID_PARAMETERS: return "XLINK_PLATFORM_INVALID_PARAMETERS";
        default: return "";
    }
}

// ------------------------------------
// XLinkPlatform API implementation. End.
// ------------------------------------



// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------

int platformToPid(const XLinkPlatform_t platform, const XLinkDeviceState_t state) {
    if (state == XLINK_UNBOOTED) {
        switch (platform) {
            case XLINK_MYRIAD_2:  return DEFAULT_UNBOOTPID_2150;
            case XLINK_MYRIAD_X:  return DEFAULT_UNBOOTPID_2485;
            default:               return AUTO_UNBOOTED_PID;
        }
    } else if (state == XLINK_BOOTED) {
        return DEFAULT_OPENPID;
    } else if(state == XLINK_BOOTLOADER){
        return DEFAULT_BOOTLOADER_PID;
    } else if(state == XLINK_FLASH_BOOTED){
        return DEFAULT_FLASH_BOOTED_PID;
    } else if (state == XLINK_ANY_STATE) {
        switch (platform) {
            case XLINK_MYRIAD_2:  return DEFAULT_UNBOOTPID_2150;
            case XLINK_MYRIAD_X:  return DEFAULT_UNBOOTPID_2485;
            default:               return AUTO_PID;
        }
    }

    return AUTO_PID;
}

pciePlatformState_t xlinkDeviceStateToPciePlatformState(const XLinkDeviceState_t state) {
    switch (state) {
        case XLINK_ANY_STATE:  return PCIE_PLATFORM_ANY_STATE;
        case XLINK_BOOTED:     return PCIE_PLATFORM_BOOTED;
        case XLINK_UNBOOTED:   return PCIE_PLATFORM_UNBOOTED;
        default:
            return PCIE_PLATFORM_ANY_STATE;
    }
}

xLinkPlatformErrorCode_t parseUsbBootError(usbBootError_t rc) {
    switch (rc) {
        case USB_BOOT_SUCCESS:
            return XLINK_PLATFORM_SUCCESS;
        case USB_BOOT_DEVICE_NOT_FOUND:
            return XLINK_PLATFORM_DEVICE_NOT_FOUND;
        case USB_BOOT_TIMEOUT:
            return XLINK_PLATFORM_TIMEOUT;
        case USB_BOOT_ERROR:
        default:
            return XLINK_PLATFORM_ERROR;
    }
}

xLinkPlatformErrorCode_t parsePCIeHostError(pcieHostError_t rc) {
    switch (rc) {
        case PCIE_HOST_SUCCESS:
            return XLINK_PLATFORM_SUCCESS;
        case PCIE_HOST_DEVICE_NOT_FOUND:
            return XLINK_PLATFORM_DEVICE_NOT_FOUND;
        case PCIE_HOST_ERROR:
            return XLINK_PLATFORM_ERROR;
        case PCIE_HOST_TIMEOUT:
            return XLINK_PLATFORM_TIMEOUT;
        case PCIE_HOST_DRIVER_NOT_LOADED:
            return XLINK_PLATFORM_PCIE_DRIVER_NOT_LOADED;
        case PCIE_INVALID_PARAMETERS:
            return XLINK_PLATFORM_INVALID_PARAMETERS;
        default:
            return XLINK_PLATFORM_ERROR;
    }
}

xLinkPlatformErrorCode_t getPCIeDeviceName(int index,
                                                  XLinkDeviceState_t state,
                                                  const deviceDesc_t in_deviceRequirements,
                                                  deviceDesc_t* out_foundDevice) {
    ASSERT_XLINK_PLATFORM(index >= 0);
    ASSERT_XLINK_PLATFORM(out_foundDevice);
    if (in_deviceRequirements.platform == XLINK_MYRIAD_2) {
        /**
         * There is no PCIe on Myriad 2. Asserting that check
         * produces enormous amount of logs in tests.
         */
        return XLINK_PLATFORM_ERROR;
    }

    char name[XLINK_MAX_NAME_SIZE] = { 0 };

    if (strlen(in_deviceRequirements.name) > 0) {
        mv_strcpy(name, XLINK_MAX_NAME_SIZE, in_deviceRequirements.name);
    }

    pcieHostError_t pcieHostRc = pcie_find_device_port(
        index, name, XLINK_MAX_NAME_SIZE, xlinkDeviceStateToPciePlatformState(state));

    xLinkPlatformErrorCode_t xLinkRc = parsePCIeHostError(pcieHostRc);

    if(xLinkRc == XLINK_PLATFORM_SUCCESS)
    {
        if (xLinkRc == XLINK_PLATFORM_SUCCESS) {
            mv_strcpy(out_foundDevice->name, XLINK_MAX_NAME_SIZE, name);
            out_foundDevice->protocol = XLINK_PCIE;
            out_foundDevice->platform = XLINK_MYRIAD_X;
        }

    }
    return xLinkRc;
}

xLinkPlatformErrorCode_t getTcpIpDevices(const deviceDesc_t in_deviceRequirements,
                                                    deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                    unsigned int *out_amountOfFoundDevices)
{
    ASSERT_XLINK_PLATFORM(out_foundDevices);
    ASSERT_XLINK_PLATFORM(out_amountOfFoundDevices);
    if (in_deviceRequirements.platform == XLINK_MYRIAD_2) {
        /**
         * No case with TCP IP devices on TCP_IP protocol
         */
        return XLINK_PLATFORM_ERROR;
    }

    if(in_deviceRequirements.state == XLINK_UNBOOTED) {
        /**
         * There is no condition where unbooted
         * state device to be found using tcp/ip.
        */
        return XLINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    return tcpip_get_devices(in_deviceRequirements, out_foundDevices, sizeFoundDevices, out_amountOfFoundDevices);
}

// ------------------------------------
// Helpers implementation. End.
// ------------------------------------
