// project
#define MVLOG_UNIT_NAME xLinkUsb

// std
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

// libraries
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include "XLink/XLinkLog.h"
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "usb_mx_id.h"
#include "usb_host.h"
#include "usb_host_memory.h"
#include "../PlatformDeviceFd.h"

// std
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <memory>
#include <new>
#include <array>
#include <random>

// Used server side only
#if defined(__unix__)
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#endif

#if defined(__linux__)
#include <cerrno>
#include <condition_variable>
#include <linux/aio_abi.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

// Linux 6.9 UAPI, also used with older SDK headers and a backported kernel.
#ifndef FUNCTIONFS_DMABUF_ATTACH
struct usb_ffs_dmabuf_transfer_req {
    int32_t fd;
    uint32_t flags;
    uint64_t length;
} __attribute__((packed));
#define FUNCTIONFS_DMABUF_ATTACH _IOW('g', 131, int)
#define FUNCTIONFS_DMABUF_DETACH _IOW('g', 132, int)
#define FUNCTIONFS_DMABUF_TRANSFER _IOW('g', 133, struct usb_ffs_dmabuf_transfer_req)
#endif
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file {
    uint32_t flags;
    int32_t fd;
};
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct dma_buf_export_sync_file)
#endif
#endif

constexpr static int MAXIMUM_PORT_NUMBERS = 7;
using VidPid = std::pair<uint16_t, uint16_t>;
static const int MX_ID_TIMEOUT_MS = 100;
static const int MX_ID_NOPS_TIMEOUT_MS = 500;

static constexpr auto DEFAULT_OPEN_TIMEOUT = std::chrono::seconds(5);
static constexpr auto DEFAULT_WRITE_TIMEOUT = 2000;
static constexpr auto DEFAULT_READ_TIMEOUT = 2000;
static constexpr std::chrono::milliseconds DEFAULT_CONNECT_TIMEOUT{20000};
static constexpr std::chrono::milliseconds DEFAULT_SEND_FILE_TIMEOUT{10000};
static constexpr size_t SERVER_CHUNKSZ = 15 * 1024 * 1024;
static constexpr auto USB1_CHUNKSZ = 64;

static constexpr int USB_VSC_INTERFACE = 0;
static constexpr int USB_VSC_ENDPOINT_IN = 0x81;
static constexpr int USB_VSC_ENDPOINT_OUT = 0x01;

enum class UsbFunction { Any, Gate, Device };
struct UsbInterface {
    int number = -1;
    uint8_t input = 0;
    uint8_t output = 0;
    uint8_t protocol = 0;
};
static std::mutex interfaceMutex;
static std::unordered_map<libusb_device_handle*, UsbInterface> deviceInterfaces;

static constexpr int XLINK_USB_DATA_TIMEOUT = 0;

static unsigned int bulk_chunklen = DEFAULT_CHUNKSZ;
static int write_timeout = DEFAULT_WRITE_TIMEOUT;
static int initialized;
static std::atomic<bool> isServer { false };

struct UsbSetupPacket {
  uint8_t  requestType;
  uint8_t  request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
};

static UsbSetupPacket bootBootloaderPacket{
    0x00, // bmRequestType: device-directed
    0xF5, // bRequest: custom
    0x0DA1, // wValue: custom
    0x0000, // wIndex
    0 // not used
};



static std::mutex mutex;
static libusb_context* context;

int usbInitialize(void* options){
    #ifdef __ANDROID__
        // If Android, set the options as JavaVM (to default context)
        if(options != nullptr){
            libusb_set_option(NULL, libusb_option::LIBUSB_OPTION_ANDROID_JAVAVM, options);
        }
    #endif

    // // Debug
    // mvLogLevelSet(MVLOG_DEBUG);

    #if defined(_WIN32) && defined(_MSC_VER)
        return usbInitialize_customdir((void**)&context);
    #endif
    return libusb_init(&context);
}

struct pair_hash {
    template <class T1, class T2>
    std::size_t operator() (const std::pair<T1, T2> &pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};

static std::unordered_map<VidPid, XLinkDeviceState_t, pair_hash> vidPidToDeviceState = {
    {{0x03E7, 0x2485}, X_LINK_UNBOOTED},
    {{0x03E7, 0xf63b}, X_LINK_BOOTED},
    {{0x03E7, 0xf63c}, X_LINK_BOOTLOADER},
    {{0x03E7, 0xf63d}, X_LINK_FLASH_BOOTED},
    {{0x05C6, 0x901d}, X_LINK_GATE},
};

struct USBGateRequest {
    uint32_t RequestNum;
    uint32_t RequestSize;
};

struct GateResponse {
    uint32_t state;
    uint32_t protocol;
    uint32_t platform;
};

static std::string getLibusbDevicePath(libusb_device *dev);
static libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev, std::string& outMxId);
static libusb_error getLibusbDeviceGateResponse(const libusb_device_descriptor* pDesc, libusb_device *dev, GateResponse& outGateResponse, std::string& outSerial);
static const char* xlink_libusb_strerror(int x);
static bool libusbDeviceHasInterface(libusb_device* dev, UsbFunction function);
static xLinkPlatformErrorCode_t refLibusbDeviceByNameWithInterface(const char* name, UsbFunction function, libusb_device** pdev);
#ifdef _WIN32
std::string getWinUsbMxId(VidPid vidpid, libusb_device* dev);
#endif

xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) {

    // Also protects usb_mx_id_cache
    std::lock_guard<std::mutex> l(mutex);

    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto numDevices = libusb_get_device_list(context, &devs);
    if(numDevices < 0) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(static_cast<int>(numDevices)));
        return X_LINK_PLATFORM_ERROR;
    }

    // Initialize mx id cache
    usb_mx_id_cache_init();

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

            XLinkPlatform_t platform = X_LINK_MYRIAD_X;
            XLinkProtocol_t protocol = X_LINK_USB_VSC;
            std::string mxId;

            if (state == X_LINK_GATE || in_deviceRequirements.platform == X_LINK_RVC3 || in_deviceRequirements.platform == X_LINK_RVC4 ||
                in_deviceRequirements.platform == X_LINK_RVC2NG) {
                if(!libusbDeviceHasInterface(devs[i], UsbFunction::Gate)) {
                    // This may be the case on Windows with WinUSB, separate devices created for each interface
                    continue;
                }
                
                GateResponse gateResponse;
                auto gateRc = getLibusbDeviceGateResponse(&desc, devs[i], gateResponse, mxId);
                if(gateRc != LIBUSB_SUCCESS) {
                    continue;
                }

                if (gateResponse.platform == 5) {
                    platform = X_LINK_RVC2NG;
                } else if (gateResponse.platform == 4) {
                    platform = X_LINK_RVC4;
                } else {
                    platform = X_LINK_RVC3;
                }

                protocol = (XLinkProtocol_t)gateResponse.protocol;
                state = (XLinkDeviceState_t)gateResponse.state;

            } else {
                // Get device mxid
                libusb_error rc = getLibusbDeviceMxId(state, devicePath, &desc, devs[i], mxId);
                mvLog(MVLOG_DEBUG, "getLibusbDeviceMxId returned: %s", xlink_libusb_strerror(rc));
                if(state == X_LINK_BOOTED && rc == LIBUSB_ERROR_NO_DEVICE) {
                    mvLog(MVLOG_WARN,
                          "Skipping temporarily booted device %s during USB search because it disappeared while reading MXID (libusb rc: %d, msg: %s)",
                          devicePath.c_str(),
                          static_cast<int>(rc),
                          xlink_libusb_strerror(rc));
                    continue;
                }
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

            }

            // Comparisons / filters
            // compare deviceId
            std::string requiredMxId(in_deviceRequirements.mxid);
            if(requiredMxId.length() > 0 && requiredMxId != mxId){
                // Current device doesn't match the "filter"
                continue;
            }
            // compare platform
            if(in_deviceRequirements.platform != X_LINK_ANY_PLATFORM && in_deviceRequirements.platform != platform){
                // Current device doesn't match the "filter"
                continue;
            }

            // Everything passed, fillout details of found device
            out_foundDevices[numDevicesFound].status = status;
            out_foundDevices[numDevicesFound].platform = platform;
            out_foundDevices[numDevicesFound].protocol = protocol;
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

extern "C" xLinkPlatformErrorCode_t refLibusbDeviceByName(const char* name, libusb_device** pdev) {
    return refLibusbDeviceByNameWithInterface(name, UsbFunction::Any, pdev);
}

static xLinkPlatformErrorCode_t refLibusbDeviceByNameWithInterface(const char* name, UsbFunction function, libusb_device** pdev) {
    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto numDevices = libusb_get_device_list(context, &devs);
    if(numDevices < 0) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(static_cast<int>(numDevices)));
        return X_LINK_PLATFORM_ERROR;
    }

    // Loop over all usb devices, increase count only if myriad device
    bool found = false;
    for(ssize_t i = 0; i < numDevices; i++) {
        if(devs[i] == nullptr) continue;

        // Check path only
        std::string devicePath = getLibusbDevicePath(devs[i]);
        // Check if compare with name
        std::string requiredName(name);
        if(requiredName.length() > 0 && requiredName == devicePath){
            if(function != UsbFunction::Any && !libusbDeviceHasInterface(devs[i], function)) {
                continue;
            }

            // Found, increase ref and exit the loop
            libusb_ref_device(devs[i]);
            *pdev = devs[i];
            found = true;
            break;
        }
    }

    // Free list of usb devices (unref each)
    libusb_free_device_list(devs, 1);

    if(!found){
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    return X_LINK_PLATFORM_SUCCESS;
}



std::string getLibusbDevicePath(libusb_device *dev) {

    std::string devicePath = "";

    // Add bus number
    uint8_t bus = libusb_get_bus_number(dev);
    devicePath += std::to_string(bus) + ".";

    // Add all subsequent port numbers
    uint8_t portNumbers[MAXIMUM_PORT_NUMBERS];
    int count = libusb_get_port_numbers(dev, portNumbers, MAXIMUM_PORT_NUMBERS);
    if (count == LIBUSB_ERROR_OVERFLOW) {
        // shouldn't happen!
        return "<error>";
    }
    if(count == 0){
        // Only bus number is available
        return devicePath;
    }

    for (int i = 0; i < count - 1; i++){
        devicePath += std::to_string(portNumbers[i]) + ".";
    }
    devicePath += std::to_string(portNumbers[count - 1]);

    // Return the device path
    return devicePath;
}

static bool findUsbInterface(libusb_device_handle* handle, UsbFunction function, UsbInterface& result) {
    libusb_config_descriptor* config = nullptr;
    if(libusb_get_config_descriptor(libusb_get_device(handle), 0, &config) != LIBUSB_SUCCESS) return false;
    const char* expected = function == UsbFunction::Gate ? "Luxonis DepthAI Gate" : "Luxonis DepthAI Device";
    const int legacyNumber = function == UsbFunction::Gate ? 2 : 3;
    const uint8_t legacyInput = function == UsbFunction::Gate ? 0x83 : 0x84;
    const uint8_t legacyOutput = function == UsbFunction::Gate ? 0x03 : 0x04;
    bool found = false;
    for(int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const auto& interface = config->interface[i];
        for(int alt = 0; alt < interface.num_altsetting && !found; ++alt) {
            const auto& descriptor = interface.altsetting[alt];
            if(descriptor.bAlternateSetting != 0 || descriptor.bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) continue;
            UsbInterface candidate;
            candidate.number = descriptor.bInterfaceNumber;
            candidate.protocol = descriptor.bInterfaceProtocol;
            for(int ep = 0; ep < descriptor.bNumEndpoints; ++ep) {
                const auto& endpoint = descriptor.endpoint[ep];
                if((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if(endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) candidate.input = endpoint.bEndpointAddress;
                else candidate.output = endpoint.bEndpointAddress;
            }
            if(!candidate.input || !candidate.output) continue;
            unsigned char name[256] = {};
            const int length = descriptor.iInterface
                ? libusb_get_string_descriptor_ascii(handle, descriptor.iInterface, name, sizeof(name)) : 0;
            if((length > 0 && std::string(reinterpret_cast<char*>(name), length) == expected) ||
               (length <= 0 && candidate.number == legacyNumber && candidate.input == legacyInput && candidate.output == legacyOutput)) {
                result = candidate;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

static bool libusbDeviceHasInterface(libusb_device* dev, UsbFunction function) {
    libusb_device_handle* handle = nullptr;
    if(libusb_open(dev, &handle) != LIBUSB_SUCCESS) return false;
    UsbInterface interface;
    const bool found = findUsbInterface(handle, function, interface);
    libusb_close(handle);
    return found;
}

static UsbInterface deviceInterface(libusb_device_handle* handle) {
    std::lock_guard<std::mutex> lock(interfaceMutex);
    const auto found = deviceInterfaces.find(handle);
    return found == deviceInterfaces.end() ? UsbInterface{} : found->second;
}

libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev, std::string& outMxId)
{
    char mxId[XLINK_MAX_MX_ID_SIZE] = {0};

    // Default MXID - empty
    outMxId = "";

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(devicePath.c_str(), mxId);

    if(found){
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mxId);
        outMxId = std::string(mxId);
        return LIBUSB_SUCCESS;
    } else {
        // If not found, retrieve mxId

        // get serial from usb descriptor
        libusb_device_handle *handle = nullptr;
        int libusb_rc = LIBUSB_SUCCESS;

        // Retry getting MX ID for 1 second
        const std::chrono::milliseconds RETRY_TIMEOUT{1000}; // 1000ms
        const std::chrono::microseconds SLEEP_BETWEEN_RETRIES{100}; // 100us

        int tryCount = 0;
        auto t1 = std::chrono::steady_clock::now();
        do {
            tryCount++;

            // Open device - if not already
            if(handle == nullptr){
                libusb_rc = libusb_open(dev, &handle);
                if (libusb_rc < 0){
                    // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
                    mvLog(MVLOG_DEBUG, "libusb_open: %s", xlink_libusb_strerror(libusb_rc));

                    // If WIN32, access error and state == BOOTED
                    #ifdef _WIN32
                    if(libusb_rc == LIBUSB_ERROR_ACCESS && state == X_LINK_BOOTED) {
                        auto winMxId = getWinUsbMxId({pDesc->idVendor, pDesc->idProduct}, dev);
                        if(winMxId != "") {
                            strncpy(mxId, winMxId.c_str(), sizeof(mxId) - 1);
                            libusb_rc = 0;
                            break;
                        }
                    }
                    #endif

                    // retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            // if UNBOOTED state, perform mx_id retrieval procedure using small program and a read command
            if(state == X_LINK_UNBOOTED){

                // Get configuration first (From OS cache)
                int active_configuration = -1;
                if( (libusb_rc = libusb_get_configuration(handle, &active_configuration)) == 0){
                    if(active_configuration != 1){
                        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
                        if ((libusb_rc = libusb_set_configuration(handle, 1)) < 0) {
                            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", xlink_libusb_strerror(libusb_rc));

                            // retry
                            std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                            continue;
                        }
                    }
                } else {
                    // getting config failed...
                    mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", xlink_libusb_strerror(libusb_rc));

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }


                // Set to auto detach & reattach kernel driver, and ignore result (success or not supported)
                libusb_set_auto_detach_kernel_driver(handle, 1);
                // Claim interface (as we'll be doing IO on endpoints)
                if ((libusb_rc = libusb_claim_interface(handle, 0)) < 0) {
                    if(libusb_rc != LIBUSB_ERROR_BUSY){
                        mvLog(MVLOG_ERROR, "libusb_claim_interface: %s", xlink_libusb_strerror(libusb_rc));
                    } else {
                        mvLog(MVLOG_DEBUG, "libusb_claim_interface: %s", xlink_libusb_strerror(libusb_rc));
                    }
                    // retry - most likely device busy by another app
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }


                const int send_ep = 0x01;
                int transferred = 0;

                // ///////////////////////
                // Start
                // If first attempt was unsuccessful, try sending NOPs to clear out potential bad state
                if (tryCount > 1) {
                    int size = 256 * 1024 + 16;
                    uint8_t *nops = (uint8_t *)calloc(size, 1);
                    if (nops) {
                        transferred = 0;
                        libusb_rc = libusb_bulk_transfer(handle, send_ep, nops, size, &transferred, MX_ID_NOPS_TIMEOUT_MS);
                        mvLog(MVLOG_DEBUG, "Sending NOPs to %s\n", devicePath.c_str());
                        free(nops);
                        if (libusb_rc < 0 || size != transferred) {
                            mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, size);
                            // Mark as error and retry
                            libusb_rc = -1;
                            // retry
                            std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                            continue;
                        }
                    }
                }

                mvLog(MVLOG_DEBUG, "Sending MXID read to %s, try %d\n", devicePath.c_str(), tryCount);
                // WD Protection & MXID Retrieval Command
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload()), usb_mx_id_get_payload_size(), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || usb_mx_id_get_payload_size() != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, usb_mx_id_get_payload_size());
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // MXID Read
                const int recv_ep = 0x81;
                const int expectedMxIdReadSize = 9;
                uint8_t rbuf[128];
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || expectedMxIdReadSize != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, expectedMxIdReadSize);
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // WD Protection end
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload_end()), usb_mx_id_get_payload_end_size(), &transferred, MX_ID_TIMEOUT_MS);
                if (libusb_rc < 0 || usb_mx_id_get_payload_end_size() != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer (%s), transfer: %d, expected: %d", xlink_libusb_strerror(libusb_rc), transferred, usb_mx_id_get_payload_end_size());
                    // Mark as error and retry
                    libusb_rc = -1;
                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                // End
                ///////////////////////

                // Release claimed interface
                // ignore error as it doesn't matter
                libusb_release_interface(handle, 0);

                // Parse mxId into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < expectedMxIdReadSize; i++) {
                    snprintf(mxId + 2*i, 3, "%02X", rbuf[i]);
                }

                // Indicate no error
                libusb_rc = 0;

            } else {

                if( (libusb_rc = libusb_get_string_descriptor_ascii(handle, pDesc->iSerialNumber, ((uint8_t*) mxId), sizeof(mxId))) < 0){
                    mvLog(MVLOG_WARN, "Failed to get string descriptor");

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // Indicate no error
                libusb_rc = 0;

            }

        } while (libusb_rc != 0 && std::chrono::steady_clock::now() - t1 < RETRY_TIMEOUT);

        // Close opened device
        if(handle != nullptr){
            libusb_close(handle);
        }

        // On windows, if libusb_rc is LIBUSB_ERROR_ACCESS and state is X_LINK_BOOTED, some other process is using the device
        // In this case, we want the libusb error to be LIBUSB_ERROR_BUSY
        #ifdef _WIN32
        if(libusb_rc == LIBUSB_ERROR_ACCESS && state == X_LINK_BOOTED) {
            libusb_rc = LIBUSB_ERROR_BUSY;
        }
        #endif

        // if mx_id couldn't be retrieved, exit by returning error
        if(libusb_rc != 0){
            return (libusb_error) libusb_rc;
        }

        // Cache the retrieved mx_id
        // Find empty space and store this entry
        // If no empty space, don't cache (possible case: >16 devices)
        int cache_index = usb_mx_id_cache_store_entry(mxId, devicePath.c_str());
        if(cache_index >= 0){
            // debug print
            mvLog(MVLOG_DEBUG, "Cached MX ID %s at index %d", mxId, cache_index);
        } else {
            // debug print
            mvLog(MVLOG_DEBUG, "Couldn't cache MX ID %s", mxId);
        }

    }

    outMxId = std::string(mxId);
    return libusb_error::LIBUSB_SUCCESS;

}

struct XLinkGateTransaction {
    libusb_device_handle* handle = nullptr;
    UsbInterface interface;
    bool attempted = false;
    int error = LIBUSB_SUCCESS;

    XLinkGateTransaction() = default;
    XLinkGateTransaction(const XLinkGateTransaction&) = delete;
    XLinkGateTransaction& operator=(const XLinkGateTransaction&) = delete;
    ~XLinkGateTransaction() { finish(XLINK_GATE_ABORTED); }

    int open(libusb_device* device) {
        int rc = libusb_open(device, &handle);
        if(rc != LIBUSB_SUCCESS) return rc;
        rc = libusb_set_auto_detach_kernel_driver(handle, 1);
        if(rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_SUPPORTED) return rc;
        if(!findUsbInterface(handle, UsbFunction::Gate, interface)) return LIBUSB_ERROR_NOT_FOUND;
        return libusb_claim_interface(handle, interface.number);
    }

    int synchronize() {
        if(interface.protocol == 0) return LIBUSB_SUCCESS;
        if(interface.protocol != 1) return LIBUSB_ERROR_NOT_SUPPORTED;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        constexpr size_t READ_SIZE = 64 * 1024;
        constexpr size_t DISCARD_LIMIT = 16 * 1024 * 1024;
        size_t discarded = 0;
        int rc = LIBUSB_ERROR_TIMEOUT;
        std::array<unsigned char, READ_SIZE + 23> buffer;
        for(unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto attemptDeadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
            auto remaining = [&]() {
                return std::chrono::duration_cast<std::chrono::milliseconds>(attemptDeadline - std::chrono::steady_clock::now()).count();
            };
            if(remaining() <= 0) return LIBUSB_ERROR_TIMEOUT;
            std::array<unsigned char, 24> marker{};
            marker[0] = 13;
            marker[4] = 16;
            try {
                std::random_device random;
                for(size_t i = 8; i < marker.size(); ++i) marker[i] = static_cast<unsigned char>(random());
            } catch(const std::exception&) {
                return LIBUSB_ERROR_OTHER;
            }
            int transferred = 0;
            for(const int offset : {0, 8}) {
                const auto writeTimeout = remaining();
                if(writeTimeout <= 0) return LIBUSB_ERROR_TIMEOUT;
                const int size = offset == 0 ? 8 : 16;
                transferred = 0;
                rc = libusb_bulk_transfer(handle, interface.output, marker.data() + offset, size, &transferred, writeTimeout);
                if(rc != LIBUSB_SUCCESS || transferred != size) return rc == LIBUSB_SUCCESS ? LIBUSB_ERROR_IO : rc;
            }
            marker[0] = 0;
            size_t retained = 0;
            while(true) {
                if(discarded > DISCARD_LIMIT - READ_SIZE) return LIBUSB_ERROR_OVERFLOW;
                const auto readTimeout = remaining();
                if(readTimeout <= 0) {
                    rc = LIBUSB_ERROR_TIMEOUT;
                    break;
                }
                transferred = 0;
                rc = libusb_bulk_transfer(handle, interface.input, buffer.data() + retained, READ_SIZE, &transferred, readTimeout);
                if(rc != LIBUSB_SUCCESS || transferred == 0) {
                    if(rc == LIBUSB_SUCCESS) rc = LIBUSB_ERROR_IO;
                    const int reset = libusb_set_interface_alt_setting(handle, interface.number, 0);
                    if(reset != LIBUSB_SUCCESS) return rc;
                    break;
                }
                discarded += transferred;
                const size_t size = retained + transferred;
                const auto end = buffer.begin() + size;
                const auto found = std::search(buffer.begin(), end, marker.begin(), marker.end());
                if(found != end) return found + marker.size() == end ? LIBUSB_SUCCESS : LIBUSB_ERROR_IO;
                retained = std::min(size, marker.size() - 1);
                std::memmove(buffer.data(), buffer.data() + size - retained, retained);
            }
        }
        return rc == LIBUSB_SUCCESS ? LIBUSB_ERROR_TIMEOUT : rc;
    }

    int transfer(bool read, void* data, int size, int timeout) {
        if(size < 0 || timeout < 0 || (size != 0 && data == nullptr)) return LIBUSB_ERROR_INVALID_PARAM;
        if(error != LIBUSB_SUCCESS) return error;
        attempted = true;
        int transferred = 0;
        error = libusb_bulk_transfer(handle, read ? interface.input : interface.output,
                                     static_cast<unsigned char*>(data), size, &transferred, timeout);
        if(error == LIBUSB_SUCCESS && transferred != size) error = LIBUSB_ERROR_IO;
        if(error != LIBUSB_SUCCESS) {
            mvLog(MVLOG_ERROR, "Gate USB %s failed: %s (%d of %d bytes)",
                  read ? "read" : "write", libusb_error_name(error), transferred, size);
        }
        return error;
    }

    int finish(XLinkGateOutcome outcome) {
        int rc = LIBUSB_SUCCESS;
        if(handle != nullptr) {
            if(attempted && outcome == XLINK_GATE_ABORTED) {
                // Clear unread endpoint data while this client still owns the interface.
                rc = libusb_set_interface_alt_setting(handle, interface.number, 0);
            }
            libusb_close(handle);
            handle = nullptr;
        }
        return error != LIBUSB_SUCCESS ? error : rc;
    }
};

static libusb_error getLibusbDeviceGateResponse(const libusb_device_descriptor* pDesc, libusb_device *dev, GateResponse& outGateResponse, std::string& outSerial) {
    XLinkGateTransaction transaction;
    int rc = transaction.open(dev);
    if(rc != LIBUSB_SUCCESS) return static_cast<libusb_error>(rc);
    rc = transaction.synchronize();
    if(rc != LIBUSB_SUCCESS) return static_cast<libusb_error>(rc);

    USBGateRequest request = {12, 0};
    rc = transaction.transfer(false, &request, sizeof(request), DEFAULT_WRITE_TIMEOUT);
    if(rc != LIBUSB_SUCCESS) return static_cast<libusb_error>(rc);
    rc = transaction.transfer(true, &request, sizeof(request), DEFAULT_READ_TIMEOUT);
    if(rc != LIBUSB_SUCCESS) return static_cast<libusb_error>(rc);
    if(request.RequestNum == 1 && request.RequestSize == 0) {
        transaction.finish(XLINK_GATE_COMPLETE);
        return LIBUSB_ERROR_IO;
    }
    if(request.RequestNum != 0 || request.RequestSize <= sizeof(GateResponse)
       || request.RequestSize > XLINK_MAX_MX_ID_SIZE + sizeof(GateResponse)) return LIBUSB_ERROR_IO;

    std::vector<uint8_t> response(request.RequestSize);
    rc = transaction.transfer(true, response.data(), response.size(), DEFAULT_READ_TIMEOUT);
    if(rc != LIBUSB_SUCCESS) return static_cast<libusb_error>(rc);

    const size_t serialSize = response.size() - sizeof(GateResponse);
    const auto serialEnd = std::find(response.begin(), response.begin() + serialSize, 0);
    outSerial.assign(reinterpret_cast<const char*>(response.data()), std::distance(response.begin(), serialEnd));
    memcpy(&outGateResponse, response.data() + serialSize, sizeof(outGateResponse));
    transaction.finish(XLINK_GATE_COMPLETE);
    return LIBUSB_SUCCESS;
}

const char* xlink_libusb_strerror(int x) {
    return libusb_strerror((libusb_error) x);
}


static libusb_error usb_open_device(XLinkProtocol_t protocol, libusb_device *dev, uint8_t* endpoint, libusb_device_handle*& handle)
{
    struct libusb_config_descriptor *cdesc;
    const struct libusb_interface_descriptor *ifdesc;
    libusb_device_handle *h = NULL;
    int res;

    if((res = libusb_open(dev, &h)) < 0)
    {

        // On windows, if libusb_rc is LIBUSB_ERROR_ACCESS and state is X_LINK_BOOTED, some other process is using the device
        // In this case, we want the libusb error to be LIBUSB_ERROR_BUSY
        #ifdef _WIN32
        if(res == LIBUSB_ERROR_ACCESS) {

            struct libusb_device_descriptor desc;
            auto res2 = libusb_get_device_descriptor(dev, &desc);
            if (res2 < 0) {
                mvLog(MVLOG_DEBUG, "Unable to get USB device descriptor: %s", xlink_libusb_strerror(res2));
                return (libusb_error) res2;
            }

            VidPid vidpid{desc.idVendor, desc.idProduct};
            auto state = vidPidToDeviceState.at(vidpid);

            if(state == X_LINK_BOOTED) {
                res = LIBUSB_ERROR_BUSY;
            }
        }
        #endif

        mvLog(MVLOG_DEBUG, "cannot open device: %s\n", xlink_libusb_strerror(res));
        return (libusb_error) res;
    }

    // Get configuration first
    int active_configuration = -1;
    if((res = libusb_get_configuration(h, &active_configuration)) < 0){
        mvLog(MVLOG_DEBUG, "setting config 1 failed: %s\n", xlink_libusb_strerror(res));
        libusb_close(h);
        return (libusb_error) res;
    }

    // Check if set configuration call is needed
    if(active_configuration != 1){
        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
        if ((res = libusb_set_configuration(h, 1)) < 0) {
            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s\n", xlink_libusb_strerror(res));
            libusb_close(h);
            return (libusb_error) res;
        }
    }

    // Set to auto detach & reattach kernel driver, and ignore result (success or not supported)
    libusb_set_auto_detach_kernel_driver(h, 1);

    if(protocol == X_LINK_USB_EP){
        UsbInterface interface;
        if(!findUsbInterface(h, UsbFunction::Device, interface)) {
            libusb_close(h);
            return LIBUSB_ERROR_NOT_FOUND;
        }
        if((res = libusb_claim_interface(h, interface.number)) < 0){
            libusb_close(h);
            return (libusb_error) res;
        }
        {
            std::lock_guard<std::mutex> lock(interfaceMutex);
            deviceInterfaces[h] = interface;
        }
        *endpoint = interface.output;
        handle = h;
        return LIBUSB_SUCCESS;
    } else {
        if((res = libusb_claim_interface(h, USB_VSC_INTERFACE)) < 0){
           mvLog(MVLOG_DEBUG, "claiming interface %d failed: %s\n", USB_VSC_INTERFACE, xlink_libusb_strerror(res));
           libusb_close(h);
           return (libusb_error) res;
        }
    }

    if((res = libusb_get_config_descriptor(dev, 0, &cdesc)) < 0)
    {
        mvLog(MVLOG_DEBUG, "Unable to get USB config descriptor: %s\n", xlink_libusb_strerror(res));
        libusb_close(h);
        return (libusb_error) res;
    }
    ifdesc = cdesc->interface->altsetting;
    for(int i=0; i<ifdesc->bNumEndpoints; i++)
    {
        mvLog(MVLOG_DEBUG, "Found EP 0x%02x : max packet size is %u bytes",
              ifdesc->endpoint[i].bEndpointAddress, ifdesc->endpoint[i].wMaxPacketSize);
        if((ifdesc->endpoint[i].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if( !(ifdesc->endpoint[i].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) )
        {
            *endpoint = ifdesc->endpoint[i].bEndpointAddress;
            bulk_chunklen = ifdesc->endpoint[i].wMaxPacketSize;
            libusb_free_config_descriptor(cdesc);
            handle = h;
            return LIBUSB_SUCCESS;
        }
    }
    libusb_free_config_descriptor(cdesc);
    libusb_close(h);
    return LIBUSB_ERROR_ACCESS;
}

static int send_file(libusb_device_handle* h, uint8_t endpoint, const void* tx_buf, unsigned filesize,uint16_t bcdusb)
{
    using namespace std::chrono;

    uint8_t *p;
    int rc;
    int wb, twb, wbr;
    int bulk_chunklen = DEFAULT_CHUNKSZ;
    twb = 0;
    p = const_cast<uint8_t*>((const uint8_t*)tx_buf);
    int send_zlp = ((filesize % 512) == 0);

    if(bcdusb < 0x200) {
        bulk_chunklen = USB1_CHUNKSZ;
    }

    auto t1 = steady_clock::now();
    mvLog(MVLOG_DEBUG, "Performing bulk write of %u bytes...", filesize);
    while(((unsigned)twb < filesize) || send_zlp)
    {
        wb = filesize - twb;
        if(wb > bulk_chunklen)
            wb = bulk_chunklen;
        wbr = 0;
        rc = libusb_bulk_transfer(h, endpoint, p, wb, &wbr, write_timeout);
        if((rc || (wb != wbr)) && (wb != 0)) // Don't check the return code for ZLP
        {
            if(rc == LIBUSB_ERROR_NO_DEVICE) {
                // The device leaves the bus when it boots, so NO_DEVICE right after the last bytes
                // is expected. Before that it means the upload was interrupted (link reset or
                // disconnect): the device did not get the whole image and will not boot from it.
                twb += wbr;
                if((unsigned)twb < filesize) {
                    mvLog(MVLOG_ERROR, "bulk write: device disconnected after %d of %u bytes", twb, filesize);
                    return USB_BOOT_ERROR;
                }
                break;
            }
            mvLog(MVLOG_WARN, "bulk write: %s (%d bytes written, %d bytes to write)", xlink_libusb_strerror(rc), wbr, wb);
            if(rc == LIBUSB_ERROR_TIMEOUT)
                return USB_BOOT_TIMEOUT;
            else return USB_BOOT_ERROR;
        }
        if (steady_clock::now() - t1 > DEFAULT_SEND_FILE_TIMEOUT) {
            return USB_BOOT_TIMEOUT;
        }
        if(wb == 0) // ZLP just sent, last packet
            break;
        twb += wbr;
        p += wbr;
    }

#ifndef NDEBUG
    double MBpS = ((double)filesize / 1048576.) / (duration_cast<duration<float>>(steady_clock::now() - t1)).count();
    mvLog(MVLOG_DEBUG, "Successfully sent %u bytes of data in %lf ms (%lf MB/s)", filesize, duration_cast<milliseconds>(steady_clock::now() - t1).count(), MBpS);
#endif

    return 0;
}

int usb_boot(const char *addr, const void *mvcmd, unsigned size)
{
    using namespace std::chrono;

    int rc = 0;
    uint8_t endpoint;

    libusb_device *dev = nullptr;
    libusb_device_handle *h;
    uint16_t bcdusb=-1;
    libusb_error res = LIBUSB_ERROR_ACCESS;

    // Commands to enable watchdog on unbooted MX device
    static const uint8_t wdog_en_cmd[] = {
        // Header
        0x4D, 0x41, 0x32, 0x78,
        // WD Protection - start
        0x9A, 0xA8, 0x00, 0x32, 0x20, 0xAD, 0xDE, 0xD0, 0xF1,
        0x9A, 0x9C, 0x00, 0x32, 0x20, 0xFF, 0xFF, 0xFF, 0xFF,
        0x9A, 0xA8, 0x00, 0x32, 0x20, 0xAD, 0xDE, 0xD0, 0xF1,
        0x9A, 0xA4, 0x00, 0x32, 0x20, 0x01, 0x00, 0x00, 0x00,
    };

    auto t1 = steady_clock::now();
    do {
        if(refLibusbDeviceByName(addr, &dev) == X_LINK_PLATFORM_SUCCESS){
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    } while(steady_clock::now() - t1 < DEFAULT_CONNECT_TIMEOUT);

    if(dev == nullptr) {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    auto t2 = steady_clock::now();
    do {
        if((res = usb_open_device(X_LINK_USB_VSC, dev, &endpoint, h)) == LIBUSB_SUCCESS){
            break;
        }
        std::this_thread::sleep_for(milliseconds(100));
    } while(steady_clock::now() - t2 < DEFAULT_CONNECT_TIMEOUT);

    if(res == LIBUSB_SUCCESS) {
        rc = send_file(h, endpoint, wdog_en_cmd, sizeof(wdog_en_cmd), bcdusb);
        if(rc == 0) {
            rc = send_file(h, endpoint, mvcmd, size, bcdusb);
        }
        libusb_release_interface(h, 0);
        libusb_close(h);
    } else {
        if(res == LIBUSB_ERROR_ACCESS) {
            rc = X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
        } else if(res == LIBUSB_ERROR_BUSY) {
            rc = X_LINK_PLATFORM_DEVICE_BUSY;
        } else {
            rc = X_LINK_PLATFORM_ERROR;
        }
    }

    if (dev) {
        libusb_unref_device(dev);
    }

    return rc;
}



xLinkPlatformErrorCode_t usbLinkOpen(XLinkProtocol_t protocol, const char *path, libusb_device_handle*& h)
{
    using namespace std::chrono;
    if (path == NULL) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

    usbBootError_t rc = USB_BOOT_DEVICE_NOT_FOUND;
    h = nullptr;
    libusb_device *dev = nullptr;
    bool found = false;

    auto t1 = steady_clock::now();
    do {
        auto refRc = X_LINK_PLATFORM_ERROR;
        if(protocol == X_LINK_USB_EP){
            refRc = refLibusbDeviceByNameWithInterface(path, UsbFunction::Device, &dev);
        } else {
            refRc = refLibusbDeviceByName(path, &dev);
        }

        if(refRc == X_LINK_PLATFORM_SUCCESS){
            found = true;
            break;
        }
    } while(steady_clock::now() - t1 < DEFAULT_OPEN_TIMEOUT);

    if(!found) {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    uint8_t ep = 0;
    libusb_error libusb_rc = usb_open_device(protocol, dev, &ep, h);
    libusb_unref_device(dev);
    if(libusb_rc == LIBUSB_SUCCESS) {
        return X_LINK_PLATFORM_SUCCESS;
    } else if(libusb_rc == LIBUSB_ERROR_ACCESS) {
        return X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
    } else if(libusb_rc == LIBUSB_ERROR_BUSY) {
        return X_LINK_PLATFORM_DEVICE_BUSY;
    } else {
        return X_LINK_PLATFORM_ERROR;
    }
}


xLinkPlatformErrorCode_t usbLinkBootBootloader(const char *path) {

    libusb_device *dev = nullptr;
    auto refErr = refLibusbDeviceByName(path, &dev);
    if(refErr != X_LINK_PLATFORM_SUCCESS) {
        return refErr;
    }
    if(dev == NULL){
        return X_LINK_PLATFORM_ERROR;
    }
    libusb_device_handle *h = NULL;


    int libusb_rc = libusb_open(dev, &h);
    if (libusb_rc < 0) {
        libusb_unref_device(dev);
        if(libusb_rc == LIBUSB_ERROR_ACCESS) {
            return X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
        }
        return X_LINK_PLATFORM_ERROR;
    }

    // Make control transfer
    libusb_rc = libusb_control_transfer(h,
        bootBootloaderPacket.requestType,   // bmRequestType: device-directed
        bootBootloaderPacket.request,   // bRequest: custom
        bootBootloaderPacket.value, // wValue: custom
        bootBootloaderPacket.index, // wIndex
        NULL,   // data pointer
        0,      // data size
        1000    // timeout [ms]
    );

    // Ignore error and close device
    libusb_unref_device(dev);
    libusb_close(h);

    if(libusb_rc < 0) {
        return X_LINK_PLATFORM_ERROR;
    }

    return X_LINK_PLATFORM_SUCCESS;
}

void usbLinkClose(XLinkProtocol_t protocol, libusb_device_handle *f, void* fdKey)
{

    if (protocol == X_LINK_USB_EP){
        const auto interface = deviceInterface(f);
        if(interface.number >= 0) libusb_release_interface(f, interface.number);
        std::lock_guard<std::mutex> lock(interfaceMutex);
        deviceInterfaces.erase(f);
    } else {
        libusb_release_interface(f, USB_VSC_INTERFACE);
    }

    if(!usbCloseReadBufferHandle(fdKey)) libusb_close(f);
}

#if defined(__linux__)
namespace {
class FunctionFsConnection {
    enum class State { Open, Stopping, Closed };
    struct Request {
        iocb control{};
        bool pending = false;
        int64_t result = 0;
    };
    struct Endpoint {
        int fd = -1;
        aio_context_t context = 0;
        std::mutex operation;
        std::array<Request, 2> requests;
    };
    enum class DmaPhase { None, Attached, Queued, Detached };
    struct DmaTransfer {
        DmaPhase phase = DmaPhase::None;
        int fd = -1;
        int fence = -1;
    };

    Endpoint input, output;
    std::mutex stateMutex, closeMutex;
    std::condition_variable changed;
    State state = State::Open;
    unsigned activeCalls = 0;
    DmaTransfer dma;
    size_t packetSize = 0;
    std::array<uint8_t, 1024> carry;
    size_t carryBegin = 0, carryEnd = 0;

    struct Operation {
        FunctionFsConnection& connection;
        std::unique_lock<std::mutex> serial;
        bool active;
        Operation(FunctionFsConnection& connection, Endpoint& endpoint)
            : connection(connection), serial(endpoint.operation) {
            std::lock_guard<std::mutex> lock(connection.stateMutex);
            active = connection.state == State::Open;
            if(active) ++connection.activeCalls;
        }
        ~Operation() {
            if(!active) return;
            std::lock_guard<std::mutex> lock(connection.stateMutex);
            --connection.activeCalls;
            connection.changed.notify_all();
        }
    };

    static int control(int fd, unsigned long command, void* argument) {
        int result;
        do { result = ioctl(fd, command, argument); } while(result < 0 && errno == EINTR);
        return result;
    }

    bool retryAdmission(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline) {
        if(state != State::Open || std::chrono::steady_clock::now() >= deadline) return false;
        changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)),
                           [this] { return state != State::Open; });
        return state == State::Open;
    }

    int controlOpen(std::unique_lock<std::mutex>& lock, unsigned long command, void* argument) {
        const auto deadline = std::chrono::steady_clock::now() + DEFAULT_OPEN_TIMEOUT;
        while(state == State::Open) {
            const int result = control(output.fd, command, argument);
            if(result == 0 || (errno != EBUSY && errno != EAGAIN)) return result;
            if(!retryAdmission(lock, deadline)) break;
        }
        return -1;
    }

    bool submit(std::unique_lock<std::mutex>& lock, Endpoint& endpoint, size_t index, const void* buffer, size_t size, bool read) {
        auto& request = endpoint.requests[index];
        request.control = iocb{};
        request.control.aio_fildes = endpoint.fd;
        request.control.aio_lio_opcode = read ? IOCB_CMD_PREAD : IOCB_CMD_PWRITE;
        request.control.aio_buf = reinterpret_cast<uintptr_t>(buffer);
        request.control.aio_nbytes = size;
        request.result = -ECANCELED;
        const auto deadline = std::chrono::steady_clock::now() + DEFAULT_OPEN_TIMEOUT;
        while(state == State::Open) {
            iocb* submitted = &request.control;
            const long count = syscall(SYS_io_submit, endpoint.context, long{1}, &submitted);
            if(count == 1) {
                request.pending = true;
                return true;
            }
            request.result = count < 0 ? -errno : -EAGAIN;
            if(request.result == -EINTR) continue;
            if(request.result != -EAGAIN || !retryAdmission(lock, deadline)) break;
        }
        return false;
    }

    bool drain(Endpoint& endpoint, bool exact) {
        bool valid = true;
        for(;;) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if(std::none_of(endpoint.requests.begin(), endpoint.requests.end(), [](const Request& request) { return request.pending; }))
                    return valid;
            }
            std::array<io_event, 2> events{};
            const long count = syscall(SYS_io_getevents, endpoint.context, long{1}, long{2}, events.data(), static_cast<timespec*>(nullptr));
            if(count < 0 && errno == EINTR) continue;
            std::unique_lock<std::mutex> lock(stateMutex);
            if(count <= 0) {
                mvLog(MVLOG_ERROR, "FunctionFS AIO completion failed: %s", std::strerror(errno));
                valid = false;
                stopLocked();
                // An observation failure does not return kernel-owned buffers to their callers.
                changed.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
            for(long i = 0; i < count; ++i) {
                auto found = std::find_if(endpoint.requests.begin(), endpoint.requests.end(), [&](const Request& request) {
                    return request.pending && events[i].obj == reinterpret_cast<uintptr_t>(&request.control);
                });
                if(found == endpoint.requests.end()) {
                    mvLog(MVLOG_ERROR, "FunctionFS returned an unknown AIO completion");
                    valid = false;
                    stopLocked();
                    continue;
                }
                found->pending = false;
                found->result = events[i].res;
                // Older FunctionFS kernels repeat the primary result in res2.
                if((events[i].res2 != 0 && events[i].res2 != events[i].res) || events[i].res > static_cast<int64_t>(found->control.aio_nbytes)
                   || (exact && events[i].res != static_cast<int64_t>(found->control.aio_nbytes))) {
                    valid = false;
                    stopLocked();
                }
            }
        }
    }

    int64_t transfer(Endpoint& endpoint, void* buffer, size_t size, bool read) {
        const auto deadline = std::chrono::steady_clock::now() + DEFAULT_OPEN_TIMEOUT;
        for(;;) {
            std::unique_lock<std::mutex> lock(stateMutex);
            if(!submit(lock, endpoint, 0, buffer, size, read)) return -1;
            lock.unlock();
            if(!drain(endpoint, false)) return -1;
            const auto result = endpoint.requests[0].result;
            if(result != -EAGAIN && result != -EBUSY) return result;
            lock.lock();
            if(!retryAdmission(lock, deadline)) return -1;
        }
    }

    bool readPacketSize() {
        if(packetSize) return true;
        const auto deadline = std::chrono::steady_clock::now() + DEFAULT_OPEN_TIMEOUT;
        std::unique_lock<std::mutex> lock(stateMutex);
        while(state == State::Open) {
            usb_endpoint_descriptor descriptor{};
            if(control(input.fd, FUNCTIONFS_ENDPOINT_DESC, &descriptor) == 0) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&descriptor.wMaxPacketSize);
                const size_t size = bytes[0] | (static_cast<size_t>(bytes[1]) << 8);
                if((descriptor.bEndpointAddress & USB_DIR_IN) || (descriptor.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK
                   || (size != 8 && size != 16 && size != 32 && size != 64 && size != 512 && size != 1024)) return false;
                packetSize = size;
                return true;
            }
            if((errno != EAGAIN && errno != ESHUTDOWN) || !retryAdmission(lock, deadline)) break;
        }
        return false;
    }

    bool detachLocked() {
        if(dma.phase != DmaPhase::Attached && dma.phase != DmaPhase::Queued) return true;
        if(control(output.fd, FUNCTIONFS_DMABUF_DETACH, &dma.fd) < 0) {
            mvLog(MVLOG_ERROR, "FunctionFS DMA-BUF detach failed: %s", std::strerror(errno));
            return false;
        }
        dma.phase = dma.phase == DmaPhase::Queued ? DmaPhase::Detached : DmaPhase::None;
        return true;
    }

    void stopLocked() {
        if(state != State::Open) return;
        state = State::Stopping;
        for(auto* endpoint : {&input, &output}) {
            for(auto& request : endpoint->requests) {
                if(!request.pending) continue;
                io_event unused{};
                const long result = syscall(SYS_io_cancel, endpoint->context, &request.control, &unused);
                // Linux reports accepted cancellation as EINPROGRESS and posts the real completion to the ring.
                if(result < 0 && errno != EINPROGRESS && errno != EINVAL)
                    mvLog(MVLOG_ERROR, "FunctionFS AIO cancellation failed: %s", std::strerror(errno));
            }
        }
        detachLocked();
        changed.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(stateMutex);
        stopLocked();
    }

    bool waitDma() {
        bool useFence = dma.fence >= 0;
        if(useFence) {
            sync_file_info status{};
            if(control(dma.fence, SYNC_IOC_FILE_INFO, &status) == 0) {
                if(status.status != 0) return status.status == 1;
            } else {
                useFence = false;
                stop();
            }
        }
        for(;;) {
            pollfd completion{useFence ? dma.fence : dma.fd, static_cast<short>(useFence ? POLLIN : POLLOUT), 0};
            const int result = poll(&completion, 1, -1);
            if(result < 0 && errno == EINTR) continue;
            if(result > 0 && !(completion.revents & (POLLERR | POLLNVAL)) && (completion.revents & completion.events)) {
                if(!useFence) return false;
                sync_file_info status{};
                if(control(dma.fence, SYNC_IOC_FILE_INFO, &status) == 0 && status.status != 0)
                    return status.status == 1;
            }
            useFence = false;
            stop();
            // Keep the retained frame until its DMA-BUF reservation proves completion.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

public:
    static std::shared_ptr<FunctionFsConnection> open() {
        std::shared_ptr<FunctionFsConnection> connection(new(std::nothrow) FunctionFsConnection);
        if(!connection) return {};
        connection->output.fd = ::open("/dev/usb-ffs/device/ep1", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        connection->input.fd = ::open("/dev/usb-ffs/device/ep2", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if(connection->input.fd < 0 || connection->output.fd < 0) return {};
        for(auto* endpoint : {&connection->input, &connection->output}) {
            if(syscall(SYS_io_setup, unsigned{2}, &endpoint->context) < 0) return {};
        }
        return connection;
    }

    ~FunctionFsConnection() { close(); }

    bool isOpen() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return state == State::Open;
    }

    int readExact(void* buffer, int size) {
        if(size < 0 || (size && !buffer)) return -1;
        Operation operation(*this, input);
        if(!operation.active) return -1;
        if(size == 0) return 0;
        if(!readPacketSize()) { stop(); return -1; }
        auto* destination = static_cast<uint8_t*>(buffer);
        size_t remaining = static_cast<size_t>(size);
        while(remaining) {
            if(carryBegin != carryEnd) {
                const size_t count = std::min(remaining, carryEnd - carryBegin);
                std::memcpy(destination, carry.data() + carryBegin, count);
                carryBegin += count;
                destination += count;
                remaining -= count;
                continue;
            }
            const size_t aligned = std::min(remaining, SERVER_CHUNKSZ) / packetSize * packetSize;
            const auto count = transfer(input, aligned ? destination : carry.data(), aligned ? aligned : packetSize, true);
            if(count <= 0) { stop(); return -1; }
            if(aligned) {
                destination += count;
                remaining -= static_cast<size_t>(count);
            } else {
                carryBegin = 0;
                carryEnd = static_cast<size_t>(count);
            }
        }
        return size;
    }

    int writeExact(const void* buffer, int size) {
        if(size < 0 || (size && !buffer)) return -1;
        Operation operation(*this, output);
        if(!operation.active) return -1;
        const auto* source = static_cast<const uint8_t*>(buffer);
        size_t remaining = static_cast<size_t>(size);
        while(remaining) {
            const auto count = transfer(output, const_cast<uint8_t*>(source), std::min(remaining, SERVER_CHUNKSZ), false);
            if(count <= 0) { stop(); return -1; }
            source += count;
            remaining -= static_cast<size_t>(count);
        }
        return size;
    }

    int writeDma(const xLinkEvent_t& event) {
        Operation operation(*this, output);
        if(!operation.active) return -1;
        bool valid = false;
        const size_t firstTrailer = std::min(static_cast<size_t>(event.data2Size), SERVER_CHUNKSZ);
        {
            std::unique_lock<std::mutex> lock(stateMutex);
            dma.fd = static_cast<int>(reinterpret_cast<intptr_t>(event.data));
            if(controlOpen(lock, FUNCTIONFS_DMABUF_ATTACH, &dma.fd) == 0) {
                dma.phase = DmaPhase::Attached;
                if(submit(lock, output, 0, &event.header, sizeof(event.header), false)) {
                    usb_ffs_dmabuf_transfer_req request{dma.fd, 0, event.header.size - static_cast<uint64_t>(event.data2Size)};
                    if(controlOpen(lock, FUNCTIONFS_DMABUF_TRANSFER, &request) == 0) {
                        dma.phase = DmaPhase::Queued;
                        dma_buf_export_sync_file sync{DMA_BUF_SYNC_WRITE, -1};
                        if(control(dma.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) == 0) {
                            dma.fence = sync.fd;
                            valid = firstTrailer == 0 || submit(lock, output, 1, event.data2, firstTrailer, false);
                        }
                    }
                }
            }
            if(!valid) stopLocked();
        }
        if(!drain(output, true)) valid = false;
        bool submitted;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            submitted = dma.phase == DmaPhase::Queued || dma.phase == DmaPhase::Detached;
        }
        if(submitted && !waitDma()) valid = false;
        size_t offset = firstTrailer;
        while(valid && offset < static_cast<size_t>(event.data2Size)) {
            const size_t count = std::min(static_cast<size_t>(event.data2Size) - offset, SERVER_CHUNKSZ);
            const auto written = transfer(output, static_cast<uint8_t*>(event.data2) + offset, count, false);
            if(written <= 0) valid = false;
            else offset += static_cast<size_t>(written);
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if(!detachLocked()) valid = false;
            if(dma.fence >= 0) ::close(dma.fence);
            dma = DmaTransfer{};
            if(!valid) stopLocked();
        }
        return valid ? 0 : -1;
    }

    void close() {
        std::lock_guard<std::mutex> closing(closeMutex);
        std::unique_lock<std::mutex> lock(stateMutex);
        if(state == State::Closed) return;
        stopLocked();
        changed.wait(lock, [this] { return activeCalls == 0; });
        for(auto* endpoint : {&input, &output}) {
            // Destroy only empty contexts. io_destroy itself can wait indefinitely for unfinished I/O.
            if(endpoint->context) syscall(SYS_io_destroy, endpoint->context);
            endpoint->context = 0;
            if(endpoint->fd >= 0) ::close(endpoint->fd);
            endpoint->fd = -1;
        }
        state = State::Closed;
    }
};

static std::mutex functionFsMutex;
static void* functionFsKey = nullptr;
static std::shared_ptr<FunctionFsConnection> functionFsConnection;

static std::shared_ptr<FunctionFsConnection> findFunctionFsConnection(void* key) {
    std::lock_guard<std::mutex> lock(functionFsMutex);
    return key == functionFsKey ? functionFsConnection : nullptr;
}
}
#endif

extern int usbFdWrite;
extern int usbFdRead;
int usbPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(__linux__)
    if(!fd) return -1;
    std::lock_guard<std::mutex> lock(functionFsMutex);
    if(functionFsConnection) return -1;
    auto connection = FunctionFsConnection::open();
    if(!connection) return -1;
    functionFsKey = createPlatformDeviceFdKey(connection.get());
    functionFsConnection = std::move(connection);
    *fd = functionFsKey;
    isServer = true;
#elif defined(__unix__)
    // FIXME: get this info from the caller, don't hardcode here
    int outfd = open("/dev/usb-ffs/device/ep1", O_WRONLY);
    int infd = open("/dev/usb-ffs/device/ep2", O_RDONLY);

    if(outfd < 0 || infd < 0) {
        return -1;
    }

    usbFdRead = infd;
    usbFdWrite = outfd;

    isServer = true;
 
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) usbFdRead);
#endif

    return 0;
}


int usbPlatformConnect(XLinkProtocol_t protocol, const char *devPathRead, const char *devPathWrite, void **fd)
{
    std::lock_guard<std::mutex> l(mutex);
#if (!defined(USE_USB_VSC))
    #ifdef USE_LINK_JTAG
    struct sockaddr_in serv_addr;
    usbFdWrite = socket(AF_INET, SOCK_STREAM, 0);
    usbFdRead = socket(AF_INET, SOCK_STREAM, 0);
    assert(usbFdWrite >=0);
    assert(usbFdRead >=0);
    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(USB_LINK_SOCKET_PORT);

    if (connect(usbFdWrite, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        mvLog(MVLOG_ERROR, "connect(usbFdWrite,...) returned < 0\n");
        if (usbFdRead >= 0)
            close(usbFdRead);
        if (usbFdWrite >= 0)
            close(usbFdWrite);
        usbFdRead = -1;
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    return 0;

#else
    usbFdRead= open(devPathRead, O_RDWR);
    if(usbFdRead < 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
    // set tty to raw mode
    struct termios  tty;
    speed_t     spd;
    int rc;
    rc = tcgetattr(usbFdRead, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdRead, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    usbFdWrite= open(devPathWrite, O_RDWR);
    if(usbFdWrite < 0)
    {
        close(usbFdRead);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    // set tty to raw mode
    rc = tcgetattr(usbFdWrite, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdWrite, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    return 0;
#endif  /*USE_LINK_JTAG*/
#else

    libusb_device_handle* usbHandle = nullptr;
    xLinkPlatformErrorCode_t ret = usbLinkOpen(protocol, devPathWrite, usbHandle);

    if (ret != X_LINK_PLATFORM_SUCCESS)
    {
        /* could fail due to port name change */
        return ret;
    }

    // Store the usb handle and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey(usbHandle);
    usbRegisterReadBufferHandle(*fd, usbHandle);

    isServer = false;
#endif  /*USE_USB_VSC*/

    return 0;
}


int usbPlatformClose(XLinkProtocol_t protocol, void *fdKey)
{
#if defined(__linux__)
    if(auto connection = findFunctionFsConnection(fdKey)) {
        connection->close();
        std::lock_guard<std::mutex> lock(functionFsMutex);
        if(functionFsKey == fdKey) {
            destroyPlatformDeviceFdKey(fdKey);
            functionFsConnection.reset();
            functionFsKey = nullptr;
        }
        return 0;
    }
    if(isServer) return -1;
#endif
    std::lock_guard<std::mutex> l(mutex);

#ifndef USE_USB_VSC
    #ifdef USE_LINK_JTAG
    /*Nothing*/
#else
    if (usbFdRead != -1){
        close(usbFdRead);
        usbFdRead = -1;
    }
    if (usbFdWrite != -1){
        close(usbFdWrite);
        usbFdWrite = -1;
    }
#endif  /*USE_LINK_JTAG*/
#else

#if defined(__unix__) && !defined(__linux__)
    if(isServer){
        if (usbFdRead != -1){
            close(usbFdRead);
            usbFdRead = -1;
        }
        if (usbFdWrite != -1){
            close(usbFdWrite);
            usbFdWrite = -1;
        }

        if(destroyPlatformDeviceFdKey(fdKey)){
            mvLog(MVLOG_FATAL, "Cannot destroy USB Handle key: %" PRIxPTR, (uintptr_t) fdKey);
        }
        return -1;
    }
#endif

    void* tmpUsbHandle = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
        mvLog(MVLOG_FATAL, "Cannot find USB Handle by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    usbLinkClose(protocol, (libusb_device_handle *) tmpUsbHandle, fdKey);

    if(destroyPlatformDeviceFdKey(fdKey)){
        mvLog(MVLOG_FATAL, "Cannot destroy USB Handle key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }

#endif  /*USE_USB_VSC*/
    return -1;
}



int usbPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length){

    // Boot it
    int rc = usb_boot(deviceDesc->name, firmware, (unsigned)length);

    if(!rc) {
        mvLog(MVLOG_DEBUG, "Boot successful, device address %s", deviceDesc->name);
    }
    return rc;
}


// Start with 8 MiB bulk read chunks and drop down to 4 MiB or 1 MiB if OS refuses it
static constexpr int USB_READ_CHUNK_START = 8 * 1024 * 1024;
static constexpr int USB_READ_CHUNK_STEP = 4 * 1024 * 1024;
static constexpr int USB_READ_CHUNK_MIN = DEFAULT_CHUNKSZ;

static int usb_read_chunk_start()
{
    const char* value = std::getenv("XLINK_USB_READ_CHUNK_MIB");
    const int mib = value ? std::atoi(value) : 0;
    if(mib <= 0)
        return USB_READ_CHUNK_START;
    // Parentheses, because windows.h defines a min() macro
    return (std::min)(mib, USB_READ_CHUNK_START / (1024 * 1024)) * 1024 * 1024;
}

static std::atomic<int> usb_read_chunk{usb_read_chunk_start()};

int usb_read(libusb_device_handle *f, void *data, size_t size, uint8_t ep)
{
    while(size > 0)
    {
        int chunk_size = usb_read_chunk.load();
        int bt = 0, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, ep, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc == LIBUSB_ERROR_NO_MEM && bt == 0 && chunk_size > USB_READ_CHUNK_MIN)
        {
            // The transfer was refused before any data moved: read the same bytes again in smaller reads
            const int smaller = chunk_size > USB_READ_CHUNK_STEP ? USB_READ_CHUNK_STEP : USB_READ_CHUNK_MIN;
            if(usb_read_chunk.compare_exchange_strong(chunk_size, smaller))
            {
                mvLog(MVLOG_WARN, "bulk read of %d bytes: %s, using reads of %d bytes", ss, xlink_libusb_strerror(rc), smaller);
            }
            continue;
        }
        if(rc)
            return rc;
        data = ((char *)data) + bt;
        size -= bt;
    }
    return 0;
}

int usb_write(libusb_device_handle *f, const void *data, size_t size, uint8_t ep)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, ep, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc)
            return rc;
        data = (char *)data + bt;
        size -= bt;
    }
    return 0;
}

#if defined(__unix__) && !defined(__linux__)
static int usb_server_read(int fd, void* data, int size) {
    size_t totalRead = 0;
    auto* readPtr = static_cast<char*>(data);
    const size_t readSize = static_cast<size_t>(size);

    while(totalRead < readSize) {
        const size_t chunk = std::min(SERVER_CHUNKSZ, readSize - totalRead);
        const auto rc = read(fd, readPtr + totalRead, chunk);
        if(rc <= 0) {
            return -1;
        }
        totalRead += static_cast<size_t>(rc);
    }

    return static_cast<int>(totalRead);
}

static int usb_server_write(int fd, const void* data, int size) {
    size_t totalWritten = 0;
    const auto* writePtr = static_cast<const char*>(data);
    const size_t writeSize = static_cast<size_t>(size);

    while(totalWritten < writeSize) {
        const size_t chunk = std::min(SERVER_CHUNKSZ, writeSize - totalWritten);
        const auto rc = write(fd, writePtr + totalWritten, chunk);
        if(rc <= 0) {
            return -1;
        }
        totalWritten += static_cast<size_t>(rc);
    }

    return static_cast<int>(totalWritten);
}
#endif

int usbPlatformSupportsDmaBuf(void* fdKey) {
#if defined(__linux__)
    auto connection = findFunctionFsConnection(fdKey);
    return connection && connection->isOpen();
#else
    return 0;
#endif
}

int usbPlatformWriteDmaBuf(xLinkEvent_t* event) {
#if defined(__linux__)
    auto connection = findFunctionFsConnection(event->deviceHandle.xLinkFD);
    return connection ? connection->writeDma(*event) : -1;
#else
    return -1;
#endif
}

int usbPlatformRead(XLinkProtocol_t protocol, void* fdKey, void* data, int size)
{
#if defined(__linux__)
    if(auto connection = findFunctionFsConnection(fdKey)) return connection->readExact(data, size);
    if(isServer) return -1;
#endif
    int rc = 0;
#ifndef USE_USB_VSC
    int nread =  0;
#ifdef USE_LINK_JTAG
    while (nread < size){
        nread += read(usbFdWrite, &((char*)data)[nread], size - nread);
        printf("read %d %d\n", nread, size);
    }
#else
    if(usbFdRead < 0)
    {
        return -1;
    }

    while(nread < size)
    {
        int toRead = (PACKET_LENGTH && (size - nread > PACKET_LENGTH)) \
                        ? PACKET_LENGTH : size - nread;

        while(toRead > 0)
        {
            rc = read(usbFdRead, &((char*)data)[nread], toRead);
            if ( rc < 0)
            {
                return -2;
            }
            toRead -=rc;
            nread += rc;
        }
        unsigned char acknowledge = 0xEF;
        int wc = write(usbFdRead, &acknowledge, sizeof(acknowledge));
        if (wc != sizeof(acknowledge))
        {
            return -2;
        }
    }
#endif  /*USE_LINK_JTAG*/
#else

    if(isServer){
#if defined(__unix__) && !defined(__linux__)
        rc = usb_server_read(usbFdRead, data, size);
#else
        rc = -1;
#endif
    } else {
        void* tmpUsbHandle = NULL;
        if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
            mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
            return -1;
        }
        libusb_device_handle* usbHandle = (libusb_device_handle*) tmpUsbHandle;

        if(protocol == X_LINK_USB_EP) {
            const auto interface = deviceInterface(usbHandle);
            rc = interface.number < 0 ? -1 : usb_read(usbHandle, data, size, interface.input);
        } else {
            rc = usb_read(usbHandle, data, size, USB_VSC_ENDPOINT_IN);
        }
    }
#endif  /*USE_USB_VSC*/
    return rc;
}

int usbPlatformWrite(XLinkProtocol_t protocol, void *fdKey, void *data, int size)
{
#if defined(__linux__)
    if(auto connection = findFunctionFsConnection(fdKey)) return connection->writeExact(data, size);
    if(isServer) return -1;
#endif
    int rc = 0;
#ifndef USE_USB_VSC
    int byteCount = 0;
#ifdef USE_LINK_JTAG
    while (byteCount < size){
        byteCount += write(usbFdWrite, &((char*)data)[byteCount], size - byteCount);
        printf("write %d %d\n", byteCount, size);
    }
#else
    if(usbFdWrite < 0)
    {
        return -1;
    }
    while(byteCount < size)
    {
       int toWrite = (PACKET_LENGTH && (size - byteCount > PACKET_LENGTH)) \
                        ? PACKET_LENGTH:size - byteCount;
       int wc = write(usbFdWrite, ((char*)data) + byteCount, toWrite);

       if ( wc != toWrite)
       {
           return -2;
       }

       byteCount += toWrite;
       unsigned char acknowledge;
       int rc;
       rc = read(usbFdWrite, &acknowledge, sizeof(acknowledge));

       if ( rc < 0)
       {
           return -2;
       }

       if (acknowledge != 0xEF)
       {
           return -2;
       }
    }
#endif  /*USE_LINK_JTAG*/
#else

    if(isServer){
#if defined(__unix__) && !defined(__linux__)
        rc = usb_server_write(usbFdWrite, data, size);
#else
        rc = -1;
#endif
    } else {
        void* tmpUsbHandle = NULL;
        if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
            mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
            return -1;
        }
        libusb_device_handle* usbHandle = (libusb_device_handle*) tmpUsbHandle;

        if(protocol == X_LINK_USB_EP){
            const auto interface = deviceInterface(usbHandle);
            rc = interface.number < 0 ? -1 : usb_write(usbHandle, data, size, interface.output);
        } else {
            rc = usb_write(usbHandle, data, size, USB_VSC_ENDPOINT_OUT);
        }
    }
#endif  /*USE_USB_VSC*/
    return rc;
}

static int beginGate(const char* name, XLinkGateTransaction** transaction, bool synchronize)
{
    if(transaction == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    *transaction = nullptr;
    if(name == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    std::unique_ptr<XLinkGateTransaction> opened(new(std::nothrow) XLinkGateTransaction);
    if(!opened) return LIBUSB_ERROR_NO_MEM;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(context == nullptr) return LIBUSB_ERROR_NO_DEVICE;
        libusb_device* device = nullptr;
        if(refLibusbDeviceByNameWithInterface(name, UsbFunction::Gate, &device) != X_LINK_PLATFORM_SUCCESS || device == nullptr) {
            return LIBUSB_ERROR_NO_DEVICE;
        }
        const int rc = opened->open(device);
        libusb_unref_device(device);
        if(rc != LIBUSB_SUCCESS) return rc;
    }
    if(synchronize) {
        const int rc = opened->synchronize();
        if(rc != LIBUSB_SUCCESS) return rc;
    }
    *transaction = opened.release();
    return LIBUSB_SUCCESS;
}

int usbPlatformGateBegin(const char* name, XLinkGateTransaction** transaction)
{
    return beginGate(name, transaction, true);
}

int usbPlatformGateTransactionRead(XLinkGateTransaction* transaction, void* data, int size, int timeout)
{
    if(transaction == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    return transaction->transfer(true, data, size, timeout);
}

int usbPlatformGateTransactionWrite(XLinkGateTransaction* transaction, const void* data, int size, int timeout)
{
    if(transaction == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    return transaction->transfer(false, const_cast<void*>(data), size, timeout);
}

int usbPlatformGateEnd(XLinkGateTransaction* transaction, XLinkGateOutcome outcome)
{
    if(transaction == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    const int rc = transaction->finish(outcome);
    delete transaction;
    return rc;
}

int usbPlatformGateRead(const char* name, void* data, int size, int timeout)
{
    XLinkGateTransaction* transaction = nullptr;
    int rc = beginGate(name, &transaction, false);
    if(rc != LIBUSB_SUCCESS) return rc;
    rc = usbPlatformGateTransactionRead(transaction, data, size, timeout);
    usbPlatformGateEnd(transaction, rc == LIBUSB_SUCCESS ? XLINK_GATE_COMPLETE : XLINK_GATE_ABORTED);
    return rc;
}

int usbPlatformGateWrite(const char* name, void* data, int size, int timeout)
{
    XLinkGateTransaction* transaction = nullptr;
    int rc = beginGate(name, &transaction, false);
    if(rc != LIBUSB_SUCCESS) return rc;
    rc = usbPlatformGateTransactionWrite(transaction, data, size, timeout);
    usbPlatformGateEnd(transaction, rc == LIBUSB_SUCCESS ? XLINK_GATE_COMPLETE : XLINK_GATE_ABORTED);
    return rc;
}

#ifdef _WIN32
#include <initguid.h>
#include <usbiodef.h>
#pragma comment(lib, "setupapi.lib")
#include <setupapi.h>
#include <vector>

// get MxId given the vidpid and libusb device (Windows only)
// Uses the Win32 SetupDI* apis. Several cautions:
// - Movidius MyriadX usb devices often change their usb path when they load their bootloader/firmware
// - Since USB is dynamic, it is technically possible for a device to change its path at any time
std::string getWinUsbMxId(VidPid vidpid, libusb_device* dev) {
    if (dev == NULL) return {};

    // init device info vars
    HDEVINFO hDevInfoSet;
    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    // get USB host controllers; each has exactly one root hub
    hDevInfoSet = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfoSet == INVALID_HANDLE_VALUE) {
        return {};
    }

    // iterate over usb host controllers and populate list with their location path for later matching to device paths
    std::vector<std::string> hostControllerLocationPaths;
    for(int i = 0; SetupDiEnumDeviceInfo(hDevInfoSet, i, &devInfoData); i++) {
        // get location paths as a REG_MULTI_SZ
        std::string locationPaths(1023, 0);
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfoSet, &devInfoData, SPDRP_LOCATION_PATHS, NULL, (PBYTE)locationPaths.c_str(), (DWORD)locationPaths.size(), NULL)) {
            continue;
        }

        // find PCI path in the multi string and emplace to back of vector
        const auto pciPosition = locationPaths.find("PCIROOT");
        if (pciPosition == std::string::npos) {
            continue;
        }
        hostControllerLocationPaths.emplace_back(locationPaths.substr(pciPosition, strnlen_s(locationPaths.c_str() + pciPosition, locationPaths.size() - pciPosition)));
    }

    // Free dev info, return if no usb host controllers found
    SetupDiDestroyDeviceInfoList(hDevInfoSet);
    if (hostControllerLocationPaths.empty()) {
        return {};
    }

    // get USB devices
    hDevInfoSet = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfoSet == INVALID_HANDLE_VALUE) {
        return {};
    }

    // iterate over usb devices and populate with device info
    std::string goalPath{getLibusbDevicePath(dev)};
    std::string deviceId;
    for(int i = 0; SetupDiEnumDeviceInfo(hDevInfoSet, i, &devInfoData); i++) {
        // get device instance id
        char instanceId[128] {};
        if(!SetupDiGetDeviceInstanceIdA(hDevInfoSet, &devInfoData, (PSTR)instanceId, sizeof(instanceId), NULL)) {
            continue;
        }

        // get device vid, pid, and serial id
        char serialId[128] {};
        uint16_t vid = 0, pid = 0;
        if(sscanf(instanceId, "USB\\VID_%hx&PID_%hx\\%s", &vid, &pid, serialId) != 3) {
            continue;
        }

        // check if this is the device we are looking for
        if(vidpid.first != vid || vidpid.second != pid) {
            continue;
        }

        // get location paths as a REG_MULTI_SZ
        std::string locationPaths(1023, 0);
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfoSet, &devInfoData, SPDRP_LOCATION_PATHS, NULL, (PBYTE)locationPaths.c_str(), (DWORD)locationPaths.size(), NULL)) {
            continue;
        }

        // find PCI path in the multi string and isolate that path
        const auto pciPosition = locationPaths.find("PCIROOT");
        if (pciPosition == std::string::npos) {
            continue;
        }
        const auto usbPath = locationPaths.substr(pciPosition, strnlen_s(locationPaths.c_str() + pciPosition, locationPaths.size() - pciPosition));

        // find matching host controller
        const auto hostController = std::find_if(hostControllerLocationPaths.begin(), hostControllerLocationPaths.end(), [&usbPath](const std::string& candidateController) noexcept {
            // check if the usb path starts with the candidate controller path
            return usbPath.find(candidateController) == 0;
        });
        if (hostController == hostControllerLocationPaths.end()) {
            mvLog(MVLOG_WARN, "Found device with matching vid/pid but no matching USBROOT hub");
            continue;
        }

        // initialize pseudo libusb path using the host controller index +1 as the "libusb bus number"
        std::string pseudoLibUsbPath = std::to_string(std::distance(hostControllerLocationPaths.begin(), hostController) + 1);

        // there is only one root hub per host controller, it is always on port 0,
        // therefore start the search past this known root hub in the usb path
        static constexpr auto usbRootLength = sizeof("#USBROOT(0)") - 1;
        auto searchPosition{usbPath.c_str() + hostController->size() + usbRootLength};

        // parse and transform the Windows USB path to the pseudo libusb path
        int charsRead = 0;
        int port = 0;
        while (sscanf(searchPosition, "#USB(%4d)%n", &port, &charsRead) == 1) {
            searchPosition += charsRead;
            pseudoLibUsbPath += '.' + std::to_string(port);
        }

        if(pseudoLibUsbPath == goalPath) {
            deviceId = serialId;
            break;
        }
    }

    // Free dev info
    SetupDiDestroyDeviceInfoList(hDevInfoSet);

    // Return deviceId if found
    return deviceId;
}
#endif
