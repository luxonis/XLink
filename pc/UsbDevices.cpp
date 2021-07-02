// project
#include "XLink/XLinkPlatform.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"
#include "usb_mx_id.h"

// std
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

// libraries
#include <libusb-1.0/libusb.h>

constexpr static int MAXIMUM_PORT_NUMBERS = 7;
using VidPid = std::pair<uint16_t, uint16_t>;
static const char* errorMxId = "<error>";
static const int MX_ID_TIMEOUT_MS = 100;


static std::mutex mutex;
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator() (const std::pair<T1, T2> &pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};
static std::unordered_map<VidPid, XLinkDeviceState_t, pair_hash> vidPidToDeviceState = {{{1,1}, X_LINK_BOOTED}};

static std::string getLibusbDevicePath(libusb_device *dev);
static std::string getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev);
static const char* xlink_libusb_strerror(int x);

extern "C" xLinkPlatformErrorCode_t getUSBDevices(const deviceDesc_t in_deviceRequirements,
                                                     deviceDesc_t* out_foundDevices, int sizeFoundDevices,
                                                     int *out_amountOfFoundDevices) {

    std::lock_guard<std::mutex> l(mutex);

    // Get list of usb devices
    static libusb_device **devs = NULL;
    auto res = libusb_get_device_list(NULL, &devs);
    if(res != LIBUSB_SUCCESS) {
        mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", xlink_libusb_strerror(res));
    }

    // Initialize mx id cache
    usb_mx_id_cache_init();

    // Loop over all usb devices, increase count only if myriad device
    int i = 0;
    int numDevicesFound = 0;
    libusb_device* dev = nullptr;
    while ((dev = devs[i++]) != NULL) {

        if(numDevicesFound >= sizeFoundDevices){
            break;
        }

        // Get device descriptor
        struct libusb_device_descriptor desc;
        if ((res = libusb_get_device_descriptor(dev, &desc)) < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device descriptor: %s", xlink_libusb_strerror(res));
            continue;
        }

        VidPid vidpid{desc.idVendor, desc.idProduct};

        if(vidPidToDeviceState.count(vidpid) > 0){
            // Device found

            // Get device state
            XLinkDeviceState_t state = vidPidToDeviceState.at(vidpid);
            // Check if compare with state
            if(in_deviceRequirements.state != X_LINK_ANY_STATE && state != in_deviceRequirements.state){
                // Current device doesn't match the "filter"
                continue;
            }

            // Get device name
            std::string devicePath = getLibusbDevicePath(dev);
            // Check if compare with name
            std::string requiredName(in_deviceRequirements.name);
            if(requiredName.length() > 0 && requiredName != devicePath){
                // Current device doesn't match the "filter"
                continue;
            }

            // Get device mxid
            std::string mxId = getLibusbDeviceMxId(state, devicePath, &desc, dev);
            // compare with MxId
            std::string requiredMxId(in_deviceRequirements.mxid);
            if(requiredMxId.length() > 0 && requiredMxId != mxId){
                // Current device doesn't match the "filter"
                continue;
            }

            // TODO, check platform

            // Everything passed, fillout details of found device
            out_foundDevices[numDevicesFound].state = state;
            strncpy(out_foundDevices[numDevicesFound].name, devicePath.c_str(), sizeof(out_foundDevices[numDevicesFound].name));
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
    for (int i = 0; i < count - 1; i++){
        devicePath += std::to_string(portNumbers[i]) + ".";
    }
    devicePath += std::to_string(portNumbers[count - 1]);

    // Return the device path
    return devicePath;
}

std::string getLibusbDeviceMxId(XLinkDeviceState_t state, std::string devicePath, const libusb_device_descriptor* pDesc, libusb_device *dev)
{

    char mxId[XLINK_MAX_MX_ID_SIZE] = {0};

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(devicePath.c_str(), mxId);

    if(found){
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mxId);
        return std::string(mxId);
    } else {
        // If not found, retrieve mxId

        // get serial from usb descriptor
        libusb_device_handle *handle = NULL;
        int libusb_rc = LIBUSB_SUCCESS;

        // Open device
        libusb_rc = libusb_open(dev, &handle);
        if (libusb_rc < 0){
            // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
            // In all these cases, return
            // no cleanup needed
            return errorMxId;
        }


        // Retry getting MX ID for 5ms
        const std::chrono::milliseconds RETRY_TIMEOUT{5}; // 5ms
        const std::chrono::microseconds SLEEP_BETWEEN_RETRIES{100}; // 100us

        auto t1 = std::chrono::steady_clock::now();
        do {

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


                // Claim interface (as we'll be doing IO on endpoints)
                if ((libusb_rc = libusb_claim_interface(handle, 0)) < 0) {
                    if(libusb_rc != LIBUSB_ERROR_BUSY){
                        mvLog(MVLOG_ERROR, "libusb_claim_interface: %s", xlink_libusb_strerror(libusb_rc));
                    }
                    // retry - most likely device busy by another app
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }


                const int send_ep = 0x01;
                const int size = usb_mx_id_get_payload_size();
                int transferred = 0;
                if ((libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload()), size, &transferred, MX_ID_TIMEOUT_MS)) < 0) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer send: %s", xlink_libusb_strerror(libusb_rc));

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                // Transfer as mxid_read_cmd size is less than 512B it should transfer all
                if (size != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer written %d, expected %d", transferred, size);

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                const int recv_ep = 0x81;
                const int expected = 9;
                uint8_t rbuf[128];
                transferred = 0;
                if ((libusb_rc = libusb_bulk_transfer(handle, recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT_MS)) < 0) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer recv: %s", xlink_libusb_strerror(libusb_rc));

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }
                if (expected != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer read %d, expected %d", transferred, expected);

                    // retry
                    std::this_thread::sleep_for(SLEEP_BETWEEN_RETRIES);
                    continue;
                }

                // Release claimed interface
                // ignore error as it doesn't matter
                libusb_release_interface(handle, 0);

                // Parse mx_id into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < transferred; i++) {
                    sprintf(mxId + 2*i, "%02X", rbuf[i]);
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
        libusb_close(handle);

        // if mx_id couldn't be retrieved, exit by returning error
        if(libusb_rc != 0){
            return errorMxId;
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

    return std::string(mxId);

}

const char* xlink_libusb_strerror(int x) {
    return libusb_strerror((libusb_error) x);
}
