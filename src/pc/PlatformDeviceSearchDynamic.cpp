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
#include <thread>
#include <chrono>

#define MVLOG_UNIT_NAME PlatformDeviceSearchDynamic
#include "XLinkLog.h"


extern "C" xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices);

xLinkPlatformErrorCode_t XLinkPlatformFindDevicesDynamic(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, unsigned sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices, int timeoutMs, bool (*cb)(deviceDesc_t*, unsigned int)) {
    memset(out_foundDevices, sizeFoundDevices, sizeof(deviceDesc_t));
    unsigned numFoundDevices = 0;
    *out_amountOfFoundDevices = 0;

    deviceDesc_t* pFoundDevices = out_foundDevices;

    using namespace std::chrono;
    auto tstart = steady_clock::now();
    auto timeout = milliseconds(timeoutMs);
    bool unlimited = false;
    if(timeoutMs == -1) {
        unlimited = true;
    }
    const auto POOL_TIME = milliseconds(330);

    // Create TCP search context upfront

    void* tcpip_ctx;
    bool usb_vsc_available = false;
    bool tcpip_available = false;

    if(XLinkIsProtocolInitialized(X_LINK_USB_VSC)) {
        usb_vsc_available = true;
    }
    if(XLinkIsProtocolInitialized(X_LINK_TCP_IP) && tcpip_create_search_context(&tcpip_ctx, in_deviceRequirements) == X_LINK_PLATFORM_SUCCESS) {
        tcpip_available = true;
    }

    xLinkPlatformErrorCode_t status = X_LINK_PLATFORM_TIMEOUT;
    do {

        auto titeration = steady_clock::now();

        switch (in_deviceRequirements.protocol){
            case X_LINK_USB_CDC:
            case X_LINK_USB_VSC:
                if(!usb_vsc_available) {
                    return static_cast<xLinkPlatformErrorCode_t>((int) X_LINK_PLATFORM_DRIVER_NOT_LOADED+in_deviceRequirements.protocol);
                }
                // Check if protocol is initialized
                getUSBDevices(in_deviceRequirements, pFoundDevices, sizeFoundDevices, out_amountOfFoundDevices);

            /* TODO(themarpe) - reenable PCIe
            case X_LINK_PCIE:
                return getPCIeDeviceName(0, state, in_deviceRequirements, out_foundDevice);
            */

            case X_LINK_TCP_IP:
                if(!tcpip_available) {
                    return static_cast<xLinkPlatformErrorCode_t>((int) X_LINK_PLATFORM_DRIVER_NOT_LOADED+in_deviceRequirements.protocol);
                }
                return tcpip_perform_search(tcpip_ctx, pFoundDevices, sizeFoundDevices, out_amountOfFoundDevices);

            case X_LINK_ANY_PROTOCOL:

                // If USB protocol is initialized
                if(usb_vsc_available) {
                    // Find first correct USB Device
                    numFoundDevices = 0;
                    getUSBDevices(in_deviceRequirements, pFoundDevices, sizeFoundDevices, &numFoundDevices);
                    *out_amountOfFoundDevices += numFoundDevices;
                    pFoundDevices += numFoundDevices;
                    // Found enough devices, return
                    if (numFoundDevices >= sizeFoundDevices) {
                        break;
                    } else {
                        sizeFoundDevices -= numFoundDevices;
                    }
                }



                /* TODO(themarpe) - reenable PCIe
                if(XLinkIsProtocolInitialized(X_LINK_PCIE)) {
                    numFoundDevices = 0;
                    PCIe_rc = getPCIeDeviceName(0, state, in_deviceRequirements, out_foundDevice);
                    // Found enough devices, return
                    pFoundDevices += numFoundDevices;
                    if (numFoundDevices >= sizeFoundDevices) {
                        return X_LINK_PLATFORM_SUCCESS;
                    } else {
                        sizeFoundDevices -= numFoundDevices;
                    }
                    *out_amountOfFoundDevices += numFoundDevices;
                }
                */

                // Try find TCPIP device
                if(tcpip_available) {
                    numFoundDevices = 0;
                    tcpip_perform_search(tcpip_ctx, pFoundDevices, sizeFoundDevices, &numFoundDevices);
                    *out_amountOfFoundDevices += numFoundDevices;
                    pFoundDevices += numFoundDevices;
                    sizeFoundDevices -= numFoundDevices;
                    // Found enough devices, return
                    if (numFoundDevices >= sizeFoundDevices) {
                        break;
                    } else {
                        sizeFoundDevices -= numFoundDevices;
                    }
                }

                break;
            default:
                mvLog(MVLOG_WARN, "Unknown protocol");
                return X_LINK_PLATFORM_INVALID_PARAMETERS;
        }

        // Filter out duplicates - routing table will decide through which interface the packets will traverse
        // TODO(themarpe) - properly separate interfaces.
        // Either bind to interface addr, or SO_BINDTODEVICE Linux, IP_BOUND_IF macOS, and prefix interface name
        {
            deviceDesc_t* devices = out_foundDevices;
            int write_index = 0;
            for(int i = 0; i < static_cast<int>(*out_amountOfFoundDevices); i++){
                bool duplicate = false;
                for(int j = i - 1; j >= 0; j--){
                    // Check if duplicate
                    if(devices[i].protocol == devices[j].protocol && strcmp(devices[i].name, devices[j].name) == 0 && strcmp(devices[i].mxid, devices[j].mxid) == 0){
                        duplicate = true;
                        break;
                    }
                }
                if(!duplicate){
                    devices[write_index] = devices[i];
                    write_index++;
                }
            }
            *out_amountOfFoundDevices = write_index;
        }

        // do cb
        if(cb) {
            if(cb(out_foundDevices, *out_amountOfFoundDevices)) {
                // found, exit up ahead
                status = X_LINK_PLATFORM_SUCCESS;
                break;
            }
        }

        if(status == X_LINK_PLATFORM_SUCCESS) {
            break;
        }

        auto tsleep = POOL_TIME - (steady_clock::now() - titeration);

        if(tsleep >= milliseconds(1)) {
            std::this_thread::sleep_for(tsleep);
        }

    } while(steady_clock::now() - tstart < timeout || unlimited);

    // close TCP_IP search context
    if(tcpip_available) {
        tcpip_close_search_context(tcpip_ctx);
    }

    return status;
}

