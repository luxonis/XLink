// project
#define MVLOG_UNIT_NAME xLinkUsb

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
#include "../PlatformDeviceFd.h"
#include "wrap_libusb.hpp"

// std
#include <mutex>
#include <array>
#include <algorithm>
#include <string>
#include <thread>
#include <tuple>
#include <chrono>
#include <cstring>

using dp::libusb::config_descriptor;
using dp::libusb::device_handle;
using dp::libusb::device_list;
using dp::libusb::out_param;
using dp::libusb::out_param_ptr;
using dp::libusb::span;
using dp::libusb::usb_context;
using dp::libusb::usb_device;
using dp::libusb::usb_error;
using VidPid = std::pair<uint16_t, uint16_t>;

static constexpr int MAXIMUM_PORT_NUMBERS = 7;
static constexpr int MX_ID_TIMEOUT_MS = 100;
static constexpr auto DEFAULT_OPEN_TIMEOUT = std::chrono::seconds(5);
static constexpr auto DEFAULT_WRITE_TIMEOUT = 2000;
static constexpr std::chrono::milliseconds DEFAULT_CONNECT_TIMEOUT{20000};
static constexpr std::chrono::milliseconds DEFAULT_SEND_FILE_TIMEOUT{10000};
static constexpr auto USB1_CHUNKSZ = 64;

static constexpr int USB_ENDPOINT_IN = 0x81;
static constexpr int USB_ENDPOINT_OUT = 0x01;

static constexpr int XLINK_USB_DATA_TIMEOUT = 0;

static int initialized;

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

// transform a libusb error code into a XLinkPlatformErrorCode_t
xLinkPlatformErrorCode_t parseLibusbError(libusb_error) noexcept;

#if defined(_WIN32) && defined(_MSC_VER)
// support Visual C++ compiler for Windows to load libusb DLL from the same directory as the XLink code
libusb_error usbInitializeCustomdir(libusb_context** hContext) noexcept;
#endif

static std::mutex cacheMutex; // protects usb_mx_id_cache_* functions
static usb_context context;

// initialize libusb and set the context
// the return is a xLinkPlatformErrorCode_t cast to an int
int usbInitialize(void* options) noexcept {
    try {
        #ifdef __ANDROID__
            // If Android, set the options as JavaVM (to default context)
            if(options != nullptr){
                libusb_set_option(nullptr, libusb_option::LIBUSB_OPTION_ANDROID_JAVAVM, options);
            }
        #else
            (void)options;
        #endif

        // // Debug
        // mvLogLevelSet(MVLOG_DEBUG);

        // Initialize mx id cache
        {
            std::lock_guard<std::mutex> l(cacheMutex);
            usb_mx_id_cache_init();
        }

        #if defined(_WIN32) && defined(_MSC_VER)
            return parseLibusbError(usbInitializeCustomdir(out_param(context)));
        #else
            return parseLibusbError(static_cast<libusb_error>(libusb_init(out_param(context))));
        #endif
    } catch (const std::exception& ex) {
        mvLog(MVLOG_FATAL, "usbInitialize() failed: %s", ex.what());
        return X_LINK_PLATFORM_ERROR;
    }
}

static bool operator==(const std::pair<VidPid, XLinkDeviceState_t>& entry, const VidPid& vidpid) noexcept {
    return entry.first.first == vidpid.first && entry.first.second == vidpid.second;
}

static constexpr std::array<std::pair<VidPid, XLinkDeviceState_t>, 4> VID_PID_TO_DEVICE_STATE = {{
    {{0x03E7, 0x2485}, X_LINK_UNBOOTED},
    {{0x03E7, 0xf63b}, X_LINK_BOOTED},
    {{0x03E7, 0xf63c}, X_LINK_BOOTLOADER},
    {{0x03E7, 0xf63d}, X_LINK_FLASH_BOOTED},
}};

static std::string getLibusbDevicePath(const usb_device&);
static libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, const std::string& devicePath, const libusb_device_descriptor& descriptor, const usb_device& device, std::string& outMxId);
static const char* xlink_libusb_strerror(ssize_t);
#ifdef _WIN32
std::string getWinUsbMxId(const VidPid&, const usb_device&);
#endif

extern "C" xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t deviceRequirements,
                                                  deviceDesc_t* const foundDevicesBuffer, const int sizeFoundDevicesBufferEntities,
                                                  unsigned int* const resultCountFoundDevices) noexcept {
    try {
        // validate parameters
        if (foundDevicesBuffer == nullptr || resultCountFoundDevices == nullptr || sizeFoundDevicesBufferEntities <= 0) {
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
        }

        // Get list of usb devices; e.g. size() == 10 when 3 xlink devices attached to three separate USB controllers
        // If this is called immediately after usb_boot succeeds, then libusb can complain at loglevel=3 that...
        //   libusb: warning [init_device] could not get node connection information for device 'USB\VID_03E7&PID_2485\03E72485': [87] The parameter is incorrect.
        //   libusb: warning [winusb_get_device_list] failed to initialize device 'USB\VID_03E7&PID_2485\03E72485'
        //   libusb: warning [winusb_get_device_list] could not detect installation state of driver for 'USB\VID_03E7&PID_2485\03E72485': [3758096907] The device instance does not exist in the hardware tree.
        device_list deviceList{context};

        // Loop over all usb devices, persist devices only if they are known/myriad devices
        const std::string requiredPath(deviceRequirements.name);
        const std::string requiredMxId(deviceRequirements.mxid);
        span<deviceDesc_t> foundDevices{foundDevicesBuffer, static_cast<size_t>(sizeFoundDevicesBufferEntities)};
        auto* foundDevice = foundDevices.begin();
        for (auto* const candidate : deviceList) {
            // validate conditions
            if(candidate == nullptr) continue;

            // setup device i/o and query device
            try {
                // Get candidate device descriptor
                const usb_device candidateDevice{candidate};
                const auto candidateDescriptor = candidateDevice.get_device_descriptor<MVLOG_DEBUG>();

                // Filter device by known vid/pid pairs
                const auto vidpid = std::find(VID_PID_TO_DEVICE_STATE.begin(), VID_PID_TO_DEVICE_STATE.end(), VidPid{candidateDescriptor.idVendor, candidateDescriptor.idProduct});
                if(vidpid == VID_PID_TO_DEVICE_STATE.end()) {
                    // Not a known vid/pid pair
                    continue;
                }

                // Compare candidate device state with requirement state
                const XLinkDeviceState_t state = vidpid->second;
                if(deviceRequirements.state != X_LINK_ANY_STATE && state != deviceRequirements.state) {
                    // Current device doesn't match the requirement state "filter"
                    continue;
                }

                // Get device path
                const std::string devicePath = getLibusbDevicePath(candidateDevice);

                // Compare paths. If name (i.e. path) is only a hint then don't filter
                if(!deviceRequirements.nameHintOnly) {
                    if(!requiredPath.empty() && requiredPath != devicePath) {
                        // Current device doesn't match the "filter"
                        continue;
                    }
                }

                // Get device mxid
                std::string candidateMxId;
                const libusb_error rc = getLibusbDeviceMxId(state, devicePath, candidateDescriptor, candidateDevice, candidateMxId);
                mvLog(MVLOG_DEBUG, "getLibusbDeviceMxId returned: %s and mxid %s", xlink_libusb_strerror(rc), candidateMxId.c_str());

                // convert device usb status -> xlink status
                XLinkError_t status = X_LINK_SUCCESS;
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

                // Compare with MxId
                if(!requiredMxId.empty() && requiredMxId != candidateMxId){
                    // Current device doesn't match the "filter"
                    continue;
                }

                // TODO(themarpe) - check platform

                // Everything passed, fillout details of found device
                foundDevice->status = status;
                foundDevice->platform = X_LINK_MYRIAD_X;
                foundDevice->protocol = X_LINK_USB_VSC;
                foundDevice->state = state;
                foundDevice->nameHintOnly = false; // BUGBUG or should it be the input parameter?
                strncpy(foundDevice->name, devicePath.c_str(), sizeof(foundDevice->name));
                strncpy(foundDevice->mxid, candidateMxId.c_str(), sizeof(foundDevice->mxid));

                // increment to the next available device entry and check if we have enough space
                if(++foundDevice == foundDevices.end()) break;
            }
            catch(const usb_error&) {
                continue;
            }
        }

        // Write the number of found devices to the out parameter
        *resultCountFoundDevices = static_cast<unsigned int>(foundDevice - foundDevices.begin());
        return X_LINK_PLATFORM_SUCCESS;

    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception& e) {
        mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
        return X_LINK_PLATFORM_ERROR;
    }
}

// Search for usb device by libusb path; returns device if found, otherwise empty device.
// All usb errors are caught and instead of throwing, returns empty device. Other exceptions are thrown.
usb_device acquireDeviceByPath(const char* const path) {
    // validate params
    if(path == nullptr || *path == '\0') throw std::invalid_argument{"path cannot be null or empty"};

    try {
        // Get list of usb devices
        device_list deviceList{context};

        // Loop over all usb devices
        const std::string requiredPath(path);
        for(auto* const candidate : deviceList) {
            if(candidate == nullptr) continue;

            // take ownership (increments ref count)
            usb_device candidateDevice{candidate};

            // compare device path with required path
            if(requiredPath == getLibusbDevicePath(candidateDevice)) {
                return candidateDevice;
            }
        }
    }
    catch(const usb_error&) {
        // Ignore all usb errors
    }
    return {};
}

std::string getLibusbDevicePath(const usb_device& device) {
    // Add bus number
    std::string devicePath{std::to_string(device.get_bus_number())};

    // Get and append all subsequent port numbers
    const auto portNumbers = device.get_port_numbers<MVLOG_ERROR, true, MAXIMUM_PORT_NUMBERS>();
    if (portNumbers.empty()) {
        // likely a host controller/root hub directly on PCIE on Windows; otherwise shouldn't happen
        // https://github.com/libusb/libusb/blob/cc498ded18fb2c6e4506c546d0351c4ae91ef2cc/libusb/core.c#L955
        // Previous code appended only a dot
        devicePath += ".0";
    }
    else {
        // Add port numbers to path separated by a dot
        for (const auto number : portNumbers) {
            devicePath.append(1, '.').append(std::to_string(number));
        }
    }
    return devicePath;
}

// get mxId for device path from cache with thread-safety
inline bool safeGetCachedMxid(const char* devicePath, char* mxId) {
    std::lock_guard<std::mutex> l(cacheMutex);
    return usb_mx_id_cache_get_entry(devicePath, mxId);
}

// store mxID for device path to cache with thread-safety
inline int safeStoreCachedMxid(const char* devicePath, const char* mxId) {
    std::lock_guard<std::mutex> l(cacheMutex);
    return usb_mx_id_cache_store_entry(mxId, devicePath);
}

libusb_error getLibusbDeviceMxId(const XLinkDeviceState_t state, const std::string& devicePath, const libusb_device_descriptor& deviceDescriptor, const usb_device& device, std::string& outMxId)
{
    char mxId[XLINK_MAX_MX_ID_SIZE] = {0};

    // Default MXID - empty
    outMxId.clear();

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    const bool found = safeGetCachedMxid(devicePath.c_str(), mxId);

    if(found) {
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mxId);
    } else {
        // If not found, retrieve mxId

        // get serial from usb descriptor
        device_handle handle;
        libusb_error libusb_rc = LIBUSB_SUCCESS;

        // Retry getting MX ID for 15ms
        const std::chrono::milliseconds RETRY_TIMEOUT{15}; // 15ms
        const std::chrono::microseconds SLEEP_BETWEEN_RETRIES{100}; // 100us

        auto t1 = std::chrono::steady_clock::now();
        do {
            // Open device - if not already
            if(!handle) {
                try {
                    handle = device.open();
                }
                catch(const usb_error& e) {
                    // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
                    libusb_rc = static_cast<libusb_error>(e.code().value());

                    // If WIN32, access error and state == BOOTED
                    #ifdef _WIN32
                    if(libusb_rc == LIBUSB_ERROR_ACCESS && state == X_LINK_BOOTED) {
                        try {
                            const auto winMxId = getWinUsbMxId({deviceDescriptor.idVendor, deviceDescriptor.idProduct}, device);
                            if(!winMxId.empty()) {
                                strncpy(mxId, winMxId.c_str(), sizeof(mxId) - 1);
                                libusb_rc = LIBUSB_SUCCESS;
                                break;
                            }
                        }
                        catch(const std::exception&) {
                            //ignore
                        }
                    }
                    #endif

                    // retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                catch(const std::exception&) {
                    return LIBUSB_ERROR_OTHER;
                }
            }

            // if UNBOOTED state, perform mx_id retrieval procedure using small program and a read command
            if(state == X_LINK_UNBOOTED) {
                try {
                    // Get configuration first (From OS cache), Check if set configuration call is needed
                    // TODO consider sharing this whole block of code with openConfigClaimDevice()
                    const auto active_configuration = handle.get_configuration<MVLOG_DEBUG>();
                    if(active_configuration != 1){
                        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1", active_configuration);
                        handle.set_configuration(1);
                    }

                    // Set to auto detach & reattach kernel driver, and ignore result (success or not supported)
                    handle.set_auto_detach_kernel_driver<MVLOG_INFO, false>(true);

                    // Claim interface (as we'll be doing IO on endpoints)
                    // TODO consider that previous C code logged with (libusb_rc == LIBUSB_ERROR_BUSY ? MVLOG_DEBUG : MVLOG_ERROR)
                    handle.claim_interface(0);
                }
                catch(const usb_error& e) {
                    // retry - most likely device busy by another app
                    libusb_rc = static_cast<libusb_error>(e.code().value());
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                catch(const std::exception&) {
                    return LIBUSB_ERROR_OTHER;
                }

                // ///////////////////////
                // Start
                // ///////////////////////
                static constexpr int send_ep = 0x01;
                static constexpr int recv_ep = 0x81;
                static constexpr int expectedMxIdReadSize = 9;
                std::array<uint8_t, expectedMxIdReadSize> receiveBuffer;
                try {
                    // WD Protection & MXID Retrieval Command
                    handle.bulk_transfer<MVLOG_ERROR, true, MX_ID_TIMEOUT_MS>(send_ep, usb_mx_id_get_payload(), usb_mx_id_get_payload_size());
                    // MXID Read
                    handle.bulk_transfer<MVLOG_ERROR, true, MX_ID_TIMEOUT_MS>(recv_ep, receiveBuffer);
                    // WD Protection end
                    handle.bulk_transfer<MVLOG_ERROR, true, MX_ID_TIMEOUT_MS>(send_ep, usb_mx_id_get_payload_end(), usb_mx_id_get_payload_end_size());
                }
                catch(const usb_error& e) {
                    // Mark as error and retry
                    libusb_rc = static_cast<libusb_error>(e.code().value());
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                // End
                // ///////////////////////

                // Parse mxId into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                receiveBuffer[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < expectedMxIdReadSize; i++) {
                    sprintf(mxId + 2*i, "%02X", receiveBuffer[i]);
                }
                libusb_rc = LIBUSB_SUCCESS;
            }

            // when not X_LINK_UNBOOTED state, get mx_id from the device's serial number string descriptor
            else {
                try {
                    // TODO refactor try/catch broader
                    // TODO refactor to save a string directly to outMxId
                    const auto serialNumber = handle.get_string_descriptor_ascii<MVLOG_WARN, true>(deviceDescriptor.iSerialNumber);
                    serialNumber.copy(mxId, sizeof(mxId) - 1);
                    mxId[serialNumber.size()] = '\0';
                }
                catch(const usb_error& e) {
                    libusb_rc = static_cast<libusb_error>(e.code().value());
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                catch(const std::exception& e) {
                    mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
                    return LIBUSB_ERROR_OTHER;
                }
                libusb_rc = LIBUSB_SUCCESS;
            }

        } while (libusb_rc != 0 && std::chrono::steady_clock::now() - t1 < RETRY_TIMEOUT);

        // if mx_id couldn't be retrieved, exit by returning error
        if(libusb_rc != 0){
            return libusb_rc;
        }

        // Cache the retrieved mx_id
        // Find empty space and store this entry
        // If no empty space, don't cache (possible case: >16 devices)
        const int cacheIndex = safeStoreCachedMxid(devicePath.c_str(), mxId);
        if(cacheIndex >= 0){
            // debug print
            mvLog(MVLOG_DEBUG, "Cached MX ID %s at index %d", mxId, cacheIndex);
        } else {
            // debug print
            mvLog(MVLOG_DEBUG, "Couldn't cache MX ID %s", mxId);
        }
    }

    outMxId = mxId;
    return LIBUSB_SUCCESS;
}

const char* xlink_libusb_strerror(ssize_t x) {
    return libusb_strerror((libusb_error) x);
}

// get usb device handle, set configuration, claim interface, and return the first bulk OUT endpoint
// All usb errors are caught and instead of throwing, return libusb_error codes. Other exceptions are thrown.
static libusb_error openConfigClaimDevice(const usb_device& device, uint8_t& endpoint, device_handle& handle) {
    try {
        // open usb device and get candidate handle
        auto candidate = device.open();

        // Set configuration to 1; optimize to check if the device is already configured
        candidate.set_configuration(1);

        // Set to auto detach & reattach kernel driver, and ignore result (success or not supported)
        candidate.set_auto_detach_kernel_driver<MVLOG_INFO, false>(true);

        // claim interface 0; it is automatically released when handle is destructed
        candidate.claim_interface(0);

        // Get device config descriptor
        const auto cdesc = device.get_config_descriptor(0);

        // find the first bulk OUT endpoint, persist its max packet size, then return the endpoint number and candidate handle
        const span<const libusb_endpoint_descriptor> endpoints{cdesc->interface->altsetting->endpoint,
                                                               cdesc->interface->altsetting->bNumEndpoints};
        for(const auto& endpointDesc : endpoints) {
            mvLog(MVLOG_DEBUG, "Found EP 0x%02x : max packet size is %u bytes", endpointDesc.bEndpointAddress, endpointDesc.wMaxPacketSize);
            if((endpointDesc.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK
               && !(endpointDesc.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)) {
                endpoint = endpointDesc.bEndpointAddress;
                candidate.set_max_packet_size(endpoint, endpointDesc.wMaxPacketSize);
                handle = std::move(candidate);
                return LIBUSB_SUCCESS;
            }
        }
        return LIBUSB_ERROR_ACCESS;
    }
    catch(const usb_error& e) {
        // intentionally only catching usb_error, not std::exception, to avoid catching unrelated exceptions
        return static_cast<libusb_error>(e.code().value());
    }
}

// Find device by its libusb path name, open and config, return the first bulk OUT (host to device) endpoint and device handle
// * timeout: milliseconds for the entire operation, not for each individual step, timeout=0 means all steps are only tried once
// * retryOpen: boolean to retry the allocation+claim of usb resources while still within the timeout period
std::pair<device_handle, uint8_t> usbSharedOpen(const char* const devicePathName, const std::chrono::milliseconds timeout, const bool retryOpen = false) {
    using std::chrono::milliseconds;
    using std::chrono::steady_clock;

    // validate parameters
    if (devicePathName == nullptr || timeout.count() < 0) throw usb_error(LIBUSB_ERROR_INVALID_PARAM);

    // get usb device by its libusb path name
    usb_device device;
    auto t1 = steady_clock::now();
    while(!(device = acquireDeviceByPath(devicePathName)) && steady_clock::now() - t1 < timeout) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    if(!device) throw usb_error(LIBUSB_ERROR_NOT_FOUND);

    // open usb device and get first bulk OUT (host to device) endpoint
    device_handle handle;
    uint8_t endpoint = 0;
    libusb_error result = LIBUSB_ERROR_ACCESS;
    while((result = openConfigClaimDevice(device, endpoint, handle)) != LIBUSB_SUCCESS && steady_clock::now() - t1 < timeout && retryOpen) {
        std::this_thread::sleep_for(milliseconds(100));
    }
    if(result != LIBUSB_SUCCESS) throw usb_error(result);
    return {std::move(handle), endpoint};
}

// opens usb device by its libusb path name with retries, sends the boot binary, and returns the result
// a success result only means the boot binary was sent, not that the device booted successfully
// note: result is an xLinkPlatformErrorCode_t cast to an int
int usb_boot(const char* addr, const void* mvcmd, unsigned size) noexcept {
    try {
        // open usb device; get handle to device and first bulk OUT (host to device) endpoint
        const auto handleEndpoint = usbSharedOpen(addr, DEFAULT_CONNECT_TIMEOUT, true);

        // transfer boot binary to device
        handleEndpoint.first.bulk_transfer<MVLOG_ERROR, true, DEFAULT_WRITE_TIMEOUT, true, DEFAULT_SEND_FILE_TIMEOUT.count()>(handleEndpoint.second, mvcmd, size);
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return static_cast<int>(parseLibusbError(static_cast<libusb_error>(e.code().value())));
    } catch(const std::exception&) {
        return static_cast<int>(X_LINK_PLATFORM_ERROR);
    }
}

// tries one time to open a usb device by its libusb path name and returns a device handle
device_handle usbLinkOpen(const char* const path) {
    return usbSharedOpen(path, DEFAULT_OPEN_TIMEOUT, false).first;
}

xLinkPlatformErrorCode_t usbLinkBootBootloader(const char* const pathName) noexcept {
    try {
        // Get device by path
        const auto device = acquireDeviceByPath(pathName);
        if(!device) return X_LINK_PLATFORM_DEVICE_NOT_FOUND;

        // Open device to get an i/o device handle
        // Make control transfer and take no action if errors occur
        device.open().control_transfer(bootBootloaderPacket.requestType,  // bmRequestType: device-directed
                                       bootBootloaderPacket.request,      // bRequest: custom
                                       bootBootloaderPacket.value,        // wValue: custom
                                       bootBootloaderPacket.index,        // wIndex
                                       nullptr,                           // data pointer
                                       0,                                 // data size
                                       std::chrono::milliseconds(1000));
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception&) {
        return X_LINK_PLATFORM_ERROR;
    }
}

// deprecated since apis like usbPlatformClose() use RAII
void usbLinkClose(libusb_device_handle *h)
{
    // BUGBUG debugger correctly shows ref=1 when entering this function.
    // when env LIBUSB_DEBUG=3, on app exit usually get...
    //   libusb: warning [libusb_exit] device 2.0 still referenced
    //   libusb: warning [libusb_exit] device 3.0 still referenced
    if (h == nullptr) return;
    libusb_release_interface(h, 0);
    libusb_close(h);
}

int usbPlatformConnect(const char* const devPathRead, const char* const devPathWrite, void **fd) noexcept
{
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
        mvLog(MVLOG_ERROR, "connect(usbFdWrite,...) returned < 0");
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
    (void)devPathRead;
    try {
        // make device_handle safely on the heap
        auto handle = std::make_unique<device_handle>(usbLinkOpen(devPathWrite));

        // store device_handle in fd lookup container
        // (as file descriptors are reused and can cause a clash with lookups between scheduler and link).
        auto* const newKey = createPlatformDeviceFdKey(handle.get());
        if (newKey == nullptr) {
            // failed transfer of ownership to fd lookup container
            // handle will close on destruction
            return X_LINK_PLATFORM_ERROR;
        }

        // successful transfer of ownership to fd lookup container
        std::ignore = handle.release();
        *fd = newKey;
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception&) {
        return X_LINK_PLATFORM_ERROR;
    }
#endif  /*USE_USB_VSC*/
}

int usbPlatformClose(void* const fdKey) noexcept {
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
    return -1;

#else
    try {
        // extract and transfer ownership of device_handle from the fd lookup container
        std::unique_ptr<device_handle> handle(static_cast<device_handle*>(extractPlatformDeviceFdKey(fdKey)));
        if (!handle) {
            mvLog(MVLOG_FATAL, "Cannot find and destroy USB Handle by key: %" PRIxPTR, (uintptr_t)fdKey);
            return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
        }

        // close device_handle and release resources normally on destruction
        // therefore usbLinkClose(handle) is not needed
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception& e) {
        mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
#endif  /*USE_USB_VSC*/
}

// Boot it via USB
int usbPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length) noexcept {
    const int rc = usb_boot(deviceDesc->name, firmware, (unsigned)length);
    if(!rc) mvLog(MVLOG_DEBUG, "Boot binary successfully sent to device address %s", deviceDesc->name);
    return rc;
}

// transform a libusb error code into a XLinkPlatformErrorCode_t
xLinkPlatformErrorCode_t parseLibusbError(libusb_error rc) noexcept {
    xLinkPlatformErrorCode_t platformResult{X_LINK_PLATFORM_SUCCESS};
    switch (rc)
    {
    case LIBUSB_SUCCESS:
        break;
    case LIBUSB_ERROR_INVALID_PARAM:
        platformResult = X_LINK_PLATFORM_INVALID_PARAMETERS;
        break;
    case LIBUSB_ERROR_ACCESS:
        platformResult = X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
        break;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        platformResult = X_LINK_PLATFORM_DEVICE_NOT_FOUND;
        break;
    case LIBUSB_ERROR_BUSY:
        platformResult = X_LINK_PLATFORM_DEVICE_BUSY;
        break;
    case LIBUSB_ERROR_TIMEOUT:
        platformResult = X_LINK_PLATFORM_TIMEOUT;
        break;
    case LIBUSB_ERROR_IO:
    case LIBUSB_ERROR_OVERFLOW:
    case LIBUSB_ERROR_PIPE:
    case LIBUSB_ERROR_INTERRUPTED:
    case LIBUSB_ERROR_NO_MEM:
    case LIBUSB_ERROR_NOT_SUPPORTED:
    case LIBUSB_ERROR_OTHER:
    default:
        platformResult = X_LINK_PLATFORM_ERROR;
        break;
    }
    return platformResult;
}

inline intmax_t usb_read(const device_handle& f, void* const data, const size_t size) {
    return f.bulk_transfer<MVLOG_ERROR, true, XLINK_USB_DATA_TIMEOUT>(USB_ENDPOINT_IN, data, size).second;
}

inline intmax_t usb_write(const device_handle& f, const void* const data, const size_t size) {
    return f.bulk_transfer<MVLOG_ERROR, true, XLINK_USB_DATA_TIMEOUT>(USB_ENDPOINT_OUT, data, size).second;
}

int usbPlatformRead(void* const fdKey, void* data, int size) noexcept
{
#ifndef USE_USB_VSC
    int rc = 0;
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
return rc;

#else
    try {
        // get non-owning device_handle* from the fd lookup container
        auto* const handle = static_cast<device_handle*>(getPlatformDeviceFdFromKeySimple(fdKey));
        if (handle == nullptr) {
            mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
            return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
        }

        usb_read(*handle, data, size);
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception&) {
        return X_LINK_PLATFORM_ERROR;
    }
#endif  /*USE_USB_VSC*/
}

int usbPlatformWrite(void* const fdKey, void *data, int size) noexcept
{
#ifndef USE_USB_VSC
    int rc = 0;
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
    return rc;

#else
    try {
        // get non-owning device_handle* from the fd lookup container
        auto* const handle = static_cast<device_handle*>(getPlatformDeviceFdFromKeySimple(fdKey));
        if (handle == nullptr) {
            mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
            return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
        }

        usb_write(*handle, data, size);
        return X_LINK_PLATFORM_SUCCESS;
    } catch(const usb_error& e) {
        return parseLibusbError(static_cast<libusb_error>(e.code().value()));
    } catch(const std::exception&) {
        return X_LINK_PLATFORM_ERROR;
    }
#endif  /*USE_USB_VSC*/
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
std::string getWinUsbMxId(const VidPid& vidpid, const usb_device& device) {
    if (!device) return {};

    // init device info vars
    HDEVINFO hDevInfoSet;
    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    // get USB host controllers; each has exactly one root hub
    hDevInfoSet = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_HOST_CONTROLLER, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfoSet == INVALID_HANDLE_VALUE) {
        return {};
    }

    // iterate over usb host controllers and populate list with their location path for later matching to device paths
    std::vector<std::string> hostControllerLocationPaths;
    for(int i = 0; SetupDiEnumDeviceInfo(hDevInfoSet, i, &devInfoData); i++) {
        // get location paths as a REG_MULTI_SZ
        std::string locationPaths(1023, 0);
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfoSet, &devInfoData, SPDRP_LOCATION_PATHS, nullptr, (PBYTE)locationPaths.c_str(), (DWORD)locationPaths.size(), nullptr)) {
            continue;
        }

        // find PCI path in the multi string and emplace to back of vector
        const auto pciPosition = locationPaths.find("PCIROOT");
        if (pciPosition == std::string::npos) {
            continue;
        }
        hostControllerLocationPaths.emplace_back(locationPaths.substr(pciPosition, strnlen_s(locationPaths.c_str() + pciPosition, locationPaths.size() - pciPosition)));
    }

    // Free device info, return if no usb host controllers found
    SetupDiDestroyDeviceInfoList(hDevInfoSet);
    if (hostControllerLocationPaths.empty()) {
        return {};
    }

    // get USB devices
    hDevInfoSet = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfoSet == INVALID_HANDLE_VALUE) {
        return {};
    }

    // iterate over usb devices and populate with device info
    std::string goalPath{getLibusbDevicePath(device)};
    std::string deviceId;
    for(int i = 0; SetupDiEnumDeviceInfo(hDevInfoSet, i, &devInfoData); i++) {
        // get device instance id
        char instanceId[128] {};
        if(!SetupDiGetDeviceInstanceIdA(hDevInfoSet, &devInfoData, (PSTR)instanceId, sizeof(instanceId), nullptr)) {
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
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfoSet, &devInfoData, SPDRP_LOCATION_PATHS, nullptr, (PBYTE)locationPaths.c_str(), (DWORD)locationPaths.size(), nullptr)) {
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

    // Free device info
    SetupDiDestroyDeviceInfoList(hDevInfoSet);

    // Return deviceId if found
    return deviceId;
}
#endif
