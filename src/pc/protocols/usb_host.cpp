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
#include <unordered_map>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

using dai::libusb::config_descriptor;
using dai::libusb::device_handle;
using dai::libusb::device_list;
using dai::libusb::usb_context;
using dai::libusb::usb_device;
using dai::libusb::usb_error;
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

static int write_timeout = DEFAULT_WRITE_TIMEOUT;
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



static std::mutex mutex; // Also protects usb_mx_id_cache
static usb_context context;

int usbInitialize(void* options){
    std::lock_guard<std::mutex> l(mutex);

    #ifdef __ANDROID__
        // If Android, set the options as JavaVM (to default context)
        if(options != nullptr){
            libusb_set_option(NULL, libusb_option::LIBUSB_OPTION_ANDROID_JAVAVM, options);
        }
    #else
        (void)options;
    #endif

    // // Debug
    // mvLogLevelSet(MVLOG_DEBUG);

    // Initialize mx id cache
    usb_mx_id_cache_init();

    #if defined(_WIN32) && defined(_MSC_VER)
        return usbInitialize_customdir(dai::out_param_ptr<void**>(context));
    #else
        return libusb_init(dai::out_param(context));
    #endif
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
};

static std::string getLibusbDevicePath(const usb_device& dev);
static libusb_error getLibusbDeviceMxId(XLinkDeviceState_t state, const std::string& devicePath, const libusb_device_descriptor* pDesc, const usb_device& dev, std::string& outMxId);
static const char* xlink_libusb_strerror(ssize_t);
#ifdef _WIN32
std::string getWinUsbMxId(const VidPid& vidpid, const usb_device& dev);
#endif

extern "C" xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, const int sizeFoundDevices,
                                                     unsigned int *out_amountOfFoundDevices) noexcept {
    try {
        // Get list of usb devices
        std::lock_guard<std::mutex> l(mutex);
        device_list deviceList{context.get()};

        // Loop over all usb devices, persist devices only if they are known/myriad devices
        const std::string requiredName(in_deviceRequirements.name);
        const std::string requiredMxId(in_deviceRequirements.mxid);
        int numDevicesFound = 0;
        for (auto* const candidate : deviceList) {
            if(candidate == nullptr) continue;
            if(numDevicesFound >= sizeFoundDevices){
                break;
            }

            // setup device i/o and query device
            try {
                // Get device descriptor
                const usb_device usbDevice{candidate};
                const auto desc = usbDevice.get_device_descriptor<MVLOG_DEBUG>();

                // Filter device by known vid/pid pairs
                const auto vidpid = vidPidToDeviceState.find(VidPid{desc.idVendor, desc.idProduct});
                if(vidpid == vidPidToDeviceState.end()){
                    // Not a known vid/pid pair
                    continue;
                }

                // Get device state, compare with requirement state
                const XLinkDeviceState_t state = vidpid->second;
                if(in_deviceRequirements.state != X_LINK_ANY_STATE && state != in_deviceRequirements.state){
                    // Current device doesn't match the requirement state "filter"
                    continue;
                }

                // Get device path
                const std::string devicePath = getLibusbDevicePath(usbDevice);

                // Compare with name. If name is only a hint then don't filter
                if(!in_deviceRequirements.nameHintOnly){
                    if(!requiredName.empty() && requiredName != devicePath){
                        // Current device doesn't match the "filter"
                        continue;
                    }
                }

                // Get device mxid
                std::string mxId;
                const libusb_error rc = getLibusbDeviceMxId(state, devicePath, &desc, usbDevice, mxId);
                mvLog(MVLOG_DEBUG, "getLibusbDeviceMxId returned: %s", xlink_libusb_strerror(rc));

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
                if(!requiredMxId.empty() && requiredMxId != mxId){
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
                ++numDevicesFound;
            }
            catch(const usb_error&) {
                continue;
            }
        }

        // Write the number of found devices
        *out_amountOfFoundDevices = numDevicesFound;
        return X_LINK_PLATFORM_SUCCESS;
    }
    catch(const std::exception& e) {
        mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
        return X_LINK_PLATFORM_ERROR;
    }
}

// search for usb device by libusb *path* not name, increment refcount on device, return device in pdev pointer
xLinkPlatformErrorCode_t refLibusbDeviceByName(const char* path, usb_device& dev) noexcept {
    // validate params
    if (!path || !*path) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

    try {
        // Get list of usb devices
        device_list deviceList{context.get()};

        // Loop over all usb devices, increase count only if myriad device that matches the name
        // TODO does not filter by myriad devices, investigate if needed
        const std::string requiredPath(path);
        for (auto* const candidate : deviceList) {
            // usb_device takes ownership and increments ref count
            if(candidate == nullptr) continue;
            usb_device usbDevice{candidate};

            // compare device path with name
            if(requiredPath == getLibusbDevicePath(usbDevice)){
                dev = std::move(usbDevice);
                return X_LINK_PLATFORM_SUCCESS;
            }
        }
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
    catch(const usb_error& e) {
        // ignore
        mvLog(MVLOG_FATAL, "Unexpected exception: %s", e.what());
    }
    catch(const std::exception& e) {
        mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
    }
    return X_LINK_PLATFORM_ERROR;
}



std::string getLibusbDevicePath(const usb_device& dev) {
    // Add bus number
    std::string devicePath{std::to_string(dev.get_bus_number())};

    // Get and append all subsequent port numbers
    const auto portNumbers = dev.get_port_numbers<MVLOG_ERROR, true, MAXIMUM_PORT_NUMBERS>();
    if (portNumbers.empty()) {
        // Shouldn't happen! Emulate previous code by appending dot
        devicePath += '.';
    }
    else {
        // Add port numbers to path separated by a dot
        for (const auto number : portNumbers) {
            devicePath.append(1, '.').append(std::to_string(number));
        }
    }
    return devicePath;
}

libusb_error getLibusbDeviceMxId(const XLinkDeviceState_t state, const std::string& devicePath, const libusb_device_descriptor* const pDesc, const usb_device& dev, std::string& outMxId)
{
    char mxId[XLINK_MAX_MX_ID_SIZE] = {0};

    // Default MXID - empty
    outMxId.clear();

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(devicePath.c_str(), mxId);

    if(found){
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mxId);
    } else {
        // If not found, retrieve mxId

        // get serial from usb descriptor
        device_handle handle;
        int libusb_rc = LIBUSB_SUCCESS;

        // Retry getting MX ID for 15ms
        const std::chrono::milliseconds RETRY_TIMEOUT{15}; // 15ms
        const std::chrono::microseconds SLEEP_BETWEEN_RETRIES{100}; // 100us

        auto t1 = std::chrono::steady_clock::now();
        do {
            // Open device - if not already
            if(!handle){
                try {
                    handle = dev.open();
                }
                catch(const usb_error& e) {
                    // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
                    libusb_rc = e.code().value();

                    // If WIN32, access error and state == BOOTED
                    #ifdef _WIN32
                    if(libusb_rc == LIBUSB_ERROR_ACCESS && state == X_LINK_BOOTED) {
                        try {
                            const auto winMxId = getWinUsbMxId({pDesc->idVendor, pDesc->idProduct}, dev);
                            if(!winMxId.empty()) {
                                strncpy(mxId, winMxId.c_str(), sizeof(mxId) - 1);
                                libusb_rc = 0;
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
            if(state == X_LINK_UNBOOTED){
                try {
                    // Get configuration first (From OS cache), Check if set configuration call is needed
                    // TODO consider sharing this whole block of code with usb_open_device()
                    const auto active_configuration = handle.get_configuration<MVLOG_DEBUG>();
                    if(active_configuration != 1){
                        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
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
                    libusb_rc = e.code().value();
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                catch(const std::exception&) {
                    return LIBUSB_ERROR_OTHER;
                }

                // ///////////////////////
                // Start
                // ///////////////////////
                const int send_ep = 0x01;
                int transferred = 0;

                // WD Protection & MXID Retrieval Command
                transferred = 0;
                libusb_rc = libusb_bulk_transfer(handle.get(), send_ep, ((uint8_t*) usb_mx_id_get_payload()), usb_mx_id_get_payload_size(), &transferred, MX_ID_TIMEOUT_MS);
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
                libusb_rc = libusb_bulk_transfer(handle.get(), recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT_MS);
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
                libusb_rc = libusb_bulk_transfer(handle.get(), send_ep, ((uint8_t*) usb_mx_id_get_payload_end()), usb_mx_id_get_payload_end_size(), &transferred, MX_ID_TIMEOUT_MS);
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
                try {
                    handle.release_interface(0);
                }
                catch(const usb_error&) {
                    // ignore error as it doesn't matter
                }
                catch(const std::exception&) {
                    return LIBUSB_ERROR_OTHER;
                }

                // Parse mxId into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < expectedMxIdReadSize; i++) {
                    sprintf(mxId + 2*i, "%02X", rbuf[i]);
                }

                // Indicate no error
                libusb_rc = 0;

            } else {
                // when not X_LINK_UNBOOTED state, get mx_id from the device's serial number string descriptor
                try {
                    // TODO refactor try/catch broader
                    // TODO refactor to save a string directly to outMxId
                    const auto serialNumber = handle.get_string_descriptor_ascii<MVLOG_WARN, true, sizeof(mxId) - 1>(pDesc->iSerialNumber);
                    serialNumber.copy(mxId, sizeof(mxId) - 1);
                    mxId[serialNumber.size()] = '\0';
                }
                catch(const usb_error& e) {
                    libusb_rc = e.code().value();
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                catch(const std::exception& e) {
                    mvLog(MVLOG_ERROR, "Unexpected exception: %s", e.what());
                    return LIBUSB_ERROR_OTHER;
                }

                // Indicate no error
                libusb_rc = 0;
            }

        } while (libusb_rc != 0 && std::chrono::steady_clock::now() - t1 < RETRY_TIMEOUT);

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

    outMxId = mxId;
    return libusb_error::LIBUSB_SUCCESS;

}

const char* xlink_libusb_strerror(ssize_t x) {
    return libusb_strerror((libusb_error) x);
}


static libusb_error usb_open_device(const usb_device& dev, uint8_t* endpoint, device_handle& handle) noexcept
{
    try {
        // open usb device and get handle
        handle = dev.open();

        // Set configuration to 1; optimize to check if the device is already configured
        handle.set_configuration(1);

        // Set to auto detach & reattach kernel driver, and ignore result (success or not supported)
        handle.set_auto_detach_kernel_driver<MVLOG_INFO, false>(true);

        // claim interface 0
        handle.claim_interface(0);

        // Get device config descriptor
        const auto cdesc = dev.get_config_descriptor(0);

        // TODO add endpoint pointer boundary checks
        const struct libusb_interface_descriptor *ifdesc = cdesc->interface->altsetting;
        for(int i=0; i<ifdesc->bNumEndpoints; i++)
        {
            mvLog(MVLOG_DEBUG, "Found EP 0x%02x : max packet size is %u bytes",
                ifdesc->endpoint[i].bEndpointAddress, ifdesc->endpoint[i].wMaxPacketSize);
            if((ifdesc->endpoint[i].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            if( !(ifdesc->endpoint[i].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) )
            {
                *endpoint = ifdesc->endpoint[i].bEndpointAddress;
                handle.set_max_packet_size(*endpoint, ifdesc->endpoint[i].wMaxPacketSize);
                return LIBUSB_SUCCESS;
            }
        }
        return LIBUSB_ERROR_ACCESS;
    }
    catch(const usb_error& e) {
        return static_cast<libusb_error>(e.code().value());
    }
    catch(const std::exception&) {
        return LIBUSB_ERROR_OTHER;
    }
}

static int send_file(const device_handle& h, uint8_t endpoint, const void* tx_buf, unsigned filesize, uint16_t bcdusb)
{
    using namespace std::chrono;

    uint8_t *p;
    int rc;
    int wb, twb, wbr;
    int bulk_chunklen = DEFAULT_CHUNKSZ;
    twb = 0;
    p = const_cast<uint8_t*>((const uint8_t*)tx_buf);
    int send_zlp = ((filesize % 512) == 0);

    /*
    // adjust chunklen with endpoint max packet size
    if (h.get_max_packet_size(endpoint) < bulk_chunklen) {
        mvLog(MVLOG_FATAL, "bulk_chunklen %d is too big for endpoint %02x, reducing to %d",
              bulk_chunklen, endpoint, h.get_max_packet_size(endpoint));
        bulk_chunklen = h.get_max_packet_size(endpoint);
    }
    */

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
        rc = libusb_bulk_transfer(h.get(), endpoint, p, wb, &wbr, write_timeout);
        if((rc || (wb != wbr)) && (wb != 0)) // Don't check the return code for ZLP
        {
            if(rc == LIBUSB_ERROR_NO_DEVICE)
                break;
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

    usb_device dev;
    device_handle handle;
    uint16_t bcdusb = -1; // -1 has all bits set to 1, therefore the highest possible USB version
    libusb_error res = LIBUSB_ERROR_ACCESS;

    auto t1 = steady_clock::now();
    do {
        if(refLibusbDeviceByName(addr, dev) == X_LINK_PLATFORM_SUCCESS){
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    } while(steady_clock::now() - t1 < DEFAULT_CONNECT_TIMEOUT);

    if(!dev) {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    int rc = 0;
    uint8_t endpoint = 0;
    auto t2 = steady_clock::now();
    do {
        if((res = usb_open_device(dev, &endpoint, handle)) == LIBUSB_SUCCESS){
            break;
        }
        std::this_thread::sleep_for(milliseconds(100));
    } while(steady_clock::now() - t2 < DEFAULT_CONNECT_TIMEOUT);

    if(res == LIBUSB_SUCCESS) {
        // get USB specification number from device descriptor
        bcdusb = dev.get_device_descriptor().bcdUSB;
        mvLog(MVLOG_DEBUG, "USB specification version: %x.%02x", bcdusb >> 8, bcdusb & 0xff);

        rc = send_file(handle, endpoint, mvcmd, size, bcdusb);
    } else {
        if(res == LIBUSB_ERROR_ACCESS) {
            rc = X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
        } else if(res == LIBUSB_ERROR_BUSY) {
            rc = X_LINK_PLATFORM_DEVICE_BUSY;
        } else {
            rc = X_LINK_PLATFORM_ERROR;
        }
    }
    return rc;
}



xLinkPlatformErrorCode_t usbLinkOpen(const char *path, device_handle& handle)
{
    using namespace std::chrono;
    if (path == NULL) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

    handle.reset();
    usb_device dev;
    bool found = false;

    auto t1 = steady_clock::now();
    do {
        if(refLibusbDeviceByName(path, dev) == X_LINK_PLATFORM_SUCCESS){
            found = true;
            break;
        }
    } while(steady_clock::now() - t1 < DEFAULT_OPEN_TIMEOUT);

    if(!found || !dev) {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    uint8_t ep = 0;
    const libusb_error libusb_rc = usb_open_device(dev, &ep, handle);
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
    usb_device dev;
    auto refErr = refLibusbDeviceByName(path, dev);
    if(refErr != X_LINK_PLATFORM_SUCCESS) {
        return refErr;
    }
    if(!dev){
        return X_LINK_PLATFORM_ERROR;
    }

    device_handle handle;
    try {
        handle = device_handle{dev.get()};
    }
    catch(const usb_error& e) {
        if(e.code().value() == LIBUSB_ERROR_ACCESS) {
            return X_LINK_PLATFORM_INSUFFICIENT_PERMISSIONS;
        }
        return X_LINK_PLATFORM_ERROR;
    }
    catch(const std::exception&) {
        return X_LINK_PLATFORM_ERROR;
    }

    // Make control transfer
    // Ignore errors then do normal unref+close device
    if(0 > libusb_control_transfer(handle.get(),
        bootBootloaderPacket.requestType,   // bmRequestType: device-directed
        bootBootloaderPacket.request,   // bRequest: custom
        bootBootloaderPacket.value, // wValue: custom
        bootBootloaderPacket.index, // wIndex
        NULL,   // data pointer
        0,      // data size
        1000    // timeout [ms]
    )) {
        return X_LINK_PLATFORM_ERROR;
    }
    return X_LINK_PLATFORM_SUCCESS;
}

void usbLinkClose(libusb_device_handle *h)
{
    // BUGBUG expected a need to unref the original device that was used to open the handle
    // but debugger shows the ref=1 when entering this function. This is unexplained.
    // when env LIBUSB_DEBUG=3, on app exit usually get...
    //   libusb: warning [libusb_exit] device 2.0 still referenced
    //   libusb: warning [libusb_exit] device 3.0 still referenced
    libusb_release_interface(h, 0);
    //libusb_device *const temp = libusb_get_device(h);
    libusb_close(h);
    //libusb_unref_device(temp);
}



int usbPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
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
    (void)devPathRead;
    device_handle usbHandle;
    xLinkPlatformErrorCode_t ret = usbLinkOpen(devPathWrite, usbHandle);

    if (ret != X_LINK_PLATFORM_SUCCESS)
    {
        /* could fail due to port name change */
        return ret;
    }

    // TODO consider storing the device_handle in the fdKey store with std::map<device_handle> so that
    // it can automatically handle refcounting, interfaces, and closing

    // Store the usb handle and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    // release ownership (not reset) the device_handle so can be later released in usbLinkClose()
    *fd = createPlatformDeviceFdKey(usbHandle.release());

#endif  /*USE_USB_VSC*/

    return 0;
}


int usbPlatformClose(void *fdKey)
{

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

    void* tmpUsbHandle = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
        mvLog(MVLOG_FATAL, "Cannot find USB Handle by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    usbLinkClose((libusb_device_handle *) tmpUsbHandle);

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



int usb_read(libusb_device_handle *f, void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_IN,(unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc)
            return rc;
        data = ((char *)data) + bt;
        size -= bt;
    }
    return 0;
}

int usb_write(libusb_device_handle *f, const void *data, size_t size)
{
    const int chunk_size = DEFAULT_CHUNKSZ;
    while(size > 0)
    {
        int bt, ss = (int)size;
        if(ss > chunk_size)
            ss = chunk_size;
        int rc = libusb_bulk_transfer(f, USB_ENDPOINT_OUT, (unsigned char *)data, ss, &bt, XLINK_USB_DATA_TIMEOUT);
        if(rc)
            return rc;
        data = (char *)data + bt;
        size -= bt;
    }
    return 0;
}

int usbPlatformRead(void* fdKey, void* data, int size)
{
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

    void* tmpUsbHandle = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    libusb_device_handle* usbHandle = (libusb_device_handle*) tmpUsbHandle;

    rc = usb_read(usbHandle, data, size);
#endif  /*USE_USB_VSC*/
    return rc;
}

int usbPlatformWrite(void *fdKey, void *data, int size)
{
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

    void* tmpUsbHandle = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    libusb_device_handle* usbHandle = (libusb_device_handle*) tmpUsbHandle;

    rc = usb_write(usbHandle, data, size);
#endif  /*USE_USB_VSC*/
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
std::string getWinUsbMxId(const VidPid& vidpid, const usb_device& dev) {
    if (!dev) return {};

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
