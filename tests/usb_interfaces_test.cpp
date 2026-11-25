#include "usb_host.h"
#include "usb_receive_fixture.hpp"
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <map>
#include <deque>
#include <vector>
#include <algorithm>

namespace {
auto* device = reinterpret_cast<libusb_device*>(0x1000);
uintptr_t nextHandle = 0x2000;
std::map<int, libusb_device_handle*> owners;
int resets = 0, resetError = 0, transferError = 0, claimError = 0;
libusb_device* devices[] = {device, nullptr};
libusb_endpoint_descriptor endpoints[3][2]{};
libusb_interface_descriptor descriptors[3]{};
libusb_interface interfaces[3]{};
libusb_config_descriptor config{};
const char* names[] = {"Unrelated debug function", "Luxonis DepthAI Gate", "Luxonis DepthAI Device"};
int references = 0, handles = 0, configurations = 0;
int claimed = -1, released = -1;
unsigned char transferredEndpoint = 0;
bool readableNames = true, shortTransfer = false, failOpen = false;
enum class DiscoveryReply { None, Valid, ShortRequest, ShortHeader, ShortPayload, Error, ApplicationError, Empty, Truncated, Oversized };
DiscoveryReply discoveryReply = DiscoveryReply::None;
unsigned discoveryStep = 0;
enum class SyncReply { None, Echo, LateReply, SplitEcho, MissingEcho, Timeout, Retry, Flood, TrailingData, ShortWrite, ShortHeader };
SyncReply syncReply = SyncReply::None;
std::deque<std::vector<unsigned char>> syncReads;
std::vector<std::vector<unsigned char>> syncRequests;
unsigned syncReadCount = 0, userWrites = 0;
bool syncPending = false;
std::vector<unsigned char> syncHeader;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

void checkReleased() {
    require(references == 0 && handles == 0 && configurations == 0 && owners.empty(), "USB resources or interface claim leaked");
}

void layout(bool legacy = false, SyncReply sync = SyncReply::None) {
    checkReleased();
    resets = resetError = transferError = claimError = 0;
    config = {};
    config.bNumInterfaces = 3;
    config.interface = interfaces;
    for(int i = 0; i < 3; ++i) {
        descriptors[i] = {};
        descriptors[i].bInterfaceNumber = i == 0 ? 6 : i - 1 + (legacy ? 2 : 0);
        descriptors[i].bInterfaceClass = LIBUSB_CLASS_VENDOR_SPEC;
        descriptors[i].bNumEndpoints = 2;
        descriptors[i].iInterface = i + 1;
        descriptors[i].endpoint = endpoints[i];
        interfaces[i].altsetting = &descriptors[i];
        interfaces[i].num_altsetting = 1;
        for(int ep = 0; ep < 2; ++ep) {
            endpoints[i][ep] = {};
            endpoints[i][ep].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
            endpoints[i][ep].bEndpointAddress = (ep == 0 ? 0x80 : 0) | (descriptors[i].bInterfaceNumber + 1);
        }
    }
    claimed = released = -1;
    readableNames = true;
    shortTransfer = failOpen = false;
    discoveryReply = DiscoveryReply::None;
    discoveryStep = 0;
    syncReply = sync;
    descriptors[1].bInterfaceProtocol = sync == SyncReply::None ? 0 : 1;
    syncReads.clear();
    syncRequests.clear();
    syncReadCount = userWrites = 0;
    syncPending = false;
    syncHeader.clear();
    names[1] = "Luxonis DepthAI Gate";
}

void transfers() {
    unsigned char data[16]{};
    require(usbPlatformGateRead("1.10", data, sizeof(data), 10) == 0, "Gate read failed");
    require(claimed == descriptors[1].bInterfaceNumber && transferredEndpoint == endpoints[1][0].bEndpointAddress,
            "Gate read selected the wrong interface or endpoint");
    checkReleased();
    require(usbPlatformGateWrite("1.10", data, sizeof(data), 10) == 0, "Gate write failed");
    require(claimed == descriptors[1].bInterfaceNumber && transferredEndpoint == endpoints[1][1].bEndpointAddress,
            "Gate write selected the wrong interface or endpoint");
    checkReleased();
    void* key = nullptr;
    require(usbPlatformConnect(X_LINK_USB_EP, "1.10", "1.10", &key) == 0, "Device connect failed");
    require(claimed == descriptors[2].bInterfaceNumber, "Device selected the wrong interface");
    require(usbPlatformRead(X_LINK_USB_EP, key, data, sizeof(data)) == 0, "Device read failed");
    require(transferredEndpoint == endpoints[2][0].bEndpointAddress, "Device read selected the wrong endpoint");
    require(usbPlatformWrite(X_LINK_USB_EP, key, data, sizeof(data)) == 0, "Device write failed");
    require(transferredEndpoint == endpoints[2][1].bEndpointAddress, "Device write selected the wrong endpoint");
    require(resets == 0, "Successful legacy chunks reset Gate endpoints");
    usbPlatformClose(X_LINK_USB_EP, key);
    require(released == descriptors[2].bInterfaceNumber, "Device close released the wrong interface");
    checkReleased();
}

unsigned discovered() {
    deviceDesc_t requirements{};
    requirements.platform = X_LINK_ANY_PLATFORM;
    requirements.state = X_LINK_ANY_STATE;
    requirements.protocol = X_LINK_USB_EP;
    deviceDesc_t found{};
    unsigned count = 0;
    require(getUSBDevices(requirements, &found, 1, &count) == X_LINK_PLATFORM_SUCCESS, "USB discovery failed");
    if(count) {
        require(std::strcmp(found.mxid, "USB-DISCOVERY") == 0, "Discovery lost the serial");
        require(found.platform == X_LINK_RVC2NG && found.state == X_LINK_GATE, "Discovery decoded the wrong identity");
    }
    return count;
}
}  // namespace

libusb_device_handle* usb_test::openHandle() {
    ++handles;
    return reinterpret_cast<libusb_device_handle*>(++nextHandle);
}

extern "C" {
int __wrap_libusb_init(libusb_context** context) { *context = reinterpret_cast<libusb_context*>(0x3000); return 0; }
ssize_t __wrap_libusb_get_device_list(libusb_context*, libusb_device*** list) { ++references; *list = devices; return 1; }
void __wrap_libusb_free_device_list(libusb_device**, int unref) { require(unref == 1, "List references not released"); --references; }
uint8_t __wrap_libusb_get_bus_number(libusb_device*) { return 1; }
int __wrap_libusb_get_port_numbers(libusb_device*, uint8_t* ports, int) { ports[0] = 10; return 1; }
libusb_device* __wrap_libusb_ref_device(libusb_device* value) { ++references; return value; }
void __wrap_libusb_unref_device(libusb_device*) { --references; }
int __wrap_libusb_open(libusb_device*, libusb_device_handle** result) {
    if(failOpen) return LIBUSB_ERROR_ACCESS;
    *result = usb_test::openHandle();
    return 0;
}
void __wrap_libusb_close(libusb_device_handle* handle) {
    usb_test::checkClose(handle);
    for(auto it = owners.begin(); it != owners.end();) {
        if(it->second == handle) it = owners.erase(it);
        else ++it;
    }
    --handles;
}
libusb_device* __wrap_libusb_get_device(libusb_device_handle*) { return device; }
int __wrap_libusb_get_device_descriptor(libusb_device*, libusb_device_descriptor* descriptor) {
    *descriptor = {};
    descriptor->idVendor = 0x05c6;
    descriptor->idProduct = 0x901d;
    return 0;
}
int __wrap_libusb_get_config_descriptor(libusb_device*, uint8_t, libusb_config_descriptor** result) {
    ++configurations; *result = &config; return 0;
}
void __wrap_libusb_free_config_descriptor(libusb_config_descriptor*) { --configurations; }
int __wrap_libusb_get_string_descriptor_ascii(libusb_device_handle*, uint8_t index, unsigned char* result, int length) {
    if(!readableNames) return LIBUSB_ERROR_NOT_SUPPORTED;
    require(index >= 1 && index <= 3, "Unexpected string descriptor");
    auto size = std::strlen(names[index - 1]);
    require(size < static_cast<unsigned>(length), "String descriptor buffer too small");
    std::memcpy(result, names[index - 1], size);
    return size;
}
int __wrap_libusb_set_auto_detach_kernel_driver(libusb_device_handle*, int) { return 0; }
int __wrap_libusb_get_configuration(libusb_device_handle*, int* value) { *value = 1; return 0; }
int __wrap_libusb_claim_interface(libusb_device_handle* handle, int number) {
    if(claimError) return claimError;
    if(owners.count(number)) return LIBUSB_ERROR_BUSY;
    claimed = number;
    owners[number] = handle;
    return 0;
}
int __wrap_libusb_release_interface(libusb_device_handle* handle, int number) {
    require(owners.at(number) == handle, "Releasing another client's interface");
    owners.erase(number);
    released = number;
    return 0;
}
int __wrap_libusb_set_interface_alt_setting(libusb_device_handle* handle, int number, int alt) {
    require(owners.at(number) == handle && number == descriptors[1].bInterfaceNumber && alt == 0, "Reset did not own the Gate interface");
    ++resets;
    return resetError;
}
int __wrap_libusb_bulk_transfer(libusb_device_handle*, unsigned char endpoint, unsigned char* data, int length, int* actual, unsigned) {
    if(!usb_test::transferFits(data, length)) {
        *actual = 0;
        return LIBUSB_ERROR_NO_MEM;
    }
    transferredEndpoint = endpoint;
    if(syncReply != SyncReply::None && !(endpoint & LIBUSB_ENDPOINT_IN)) {
        if(length >= 8 && data[0] == 13 && data[4] == 16) {
            require(descriptors[1].bInterfaceProtocol == 1 && length == 8, "SYNC header must be a separate eight-byte transfer");
            require(syncHeader.empty(), "SYNC sent another header before its nonce");
            syncHeader.assign(data, data + length);
            *actual = length - (syncReply == SyncReply::ShortHeader ? 1 : 0);
            return 0;
        }
        if(!syncHeader.empty()) {
            require(length == 16, "SYNC nonce must be a separate sixteen-byte transfer");
            auto request = syncHeader;
            request.insert(request.end(), data, data + length);
            syncHeader.clear();
            require(std::find(syncRequests.begin(), syncRequests.end(), request) == syncRequests.end(), "SYNC reused a nonce");
            syncRequests.push_back(request);
            syncReads.clear();
            syncPending = true;
            request[0] = 0;
            if(syncReply == SyncReply::LateReply) syncReads.push_back(std::vector<unsigned char>(44, 0x5a));
            if(syncReply == SyncReply::SplitEcho) {
                syncReads.push_back({request.begin(), request.begin() + 11});
                syncReads.push_back({request.begin() + 11, request.begin() + 19});
                syncReads.push_back({request.begin() + 19, request.end()});
            } else if(syncReply == SyncReply::MissingEcho || (syncReply == SyncReply::Retry && syncRequests.size() == 1)) {
                syncReads.push_back({request.begin(), request.begin() + 20});
            } else if(syncReply != SyncReply::Timeout && syncReply != SyncReply::Flood) {
                if(syncReply == SyncReply::TrailingData) request.push_back(0xff);
                if(syncReply == SyncReply::Retry) {
                    auto stale = syncRequests.front();
                    stale[0] = 0;
                    syncReads.push_back(stale);
                }
                syncReads.push_back(request);
            }
            *actual = length - (syncReply == SyncReply::ShortWrite ? 1 : 0);
            return 0;
        }
        ++userWrites;
    }
    if(syncPending && (endpoint & LIBUSB_ENDPOINT_IN)) {
        require(length == 64 * 1024, "SYNC read is not packet-aligned");
        ++syncReadCount;
        if(syncReply == SyncReply::Flood) {
            std::memset(data, 0x5a, length);
            *actual = length;
            return 0;
        }
        if(syncReads.empty()) {
            *actual = 0;
            return LIBUSB_ERROR_TIMEOUT;
        }
        const auto response = syncReads.front();
        syncReads.pop_front();
        std::memcpy(data, response.data(), response.size());
        *actual = response.size();
        if(syncReads.empty() && syncReply != SyncReply::MissingEcho && !(syncReply == SyncReply::Retry && syncRequests.size() == 1)) syncPending = false;
        return 0;
    }
    if(transferError) return transferError;
    if(discoveryReply != DiscoveryReply::None) {
        *actual = length;
        switch(discoveryStep++) {
            case 0:
                require(length == 8 && !(endpoint & LIBUSB_ENDPOINT_IN), "Unexpected discovery request");
                if(discoveryReply == DiscoveryReply::ShortRequest) --*actual;
                break;
            case 1: {
                require(length == 8, "Unexpected discovery header size");
                uint32_t header[] = {discoveryReply == DiscoveryReply::Error ? 1u : 0u, 44};
                if(discoveryReply == DiscoveryReply::ApplicationError) { header[0] = 1; header[1] = 0; }
                if(discoveryReply == DiscoveryReply::Empty) header[1] = 0;
                if(discoveryReply == DiscoveryReply::Truncated) header[1] = 11;
                if(discoveryReply == DiscoveryReply::Oversized) header[1] = UINT32_MAX;
                std::memcpy(data, header, sizeof(header));
                if(discoveryReply == DiscoveryReply::ShortHeader) --*actual;
                break;
            }
            case 2: {
                require(length == 44, "Discovery allocated an invalid payload size");
                std::memset(data, 0, length);
                std::memcpy(data, "USB-DISCOVERY", 13);
                uint32_t identity[] = {5, 7, 5};
                std::memcpy(data + 32, identity, sizeof(identity));
                if(discoveryReply == DiscoveryReply::ShortPayload) --*actual;
                break;
            }
            default: throw std::runtime_error("Unexpected discovery transfer");
        }
        return 0;
    }
    if(endpoint & LIBUSB_ENDPOINT_IN) std::memset(data, 0x5a, length);
    *actual = length - (shortTransfer ? 1 : 0);
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        require(usbInitialize(nullptr) == 0, "USB initialization failed");
        if(argc == 2 && std::strcmp(argv[1], "memory") == 0) {
            layout();
            usb_test::checkReceiveMemory();
            checkReleased();
            return 0;
        }
        for(bool legacy : {false, true}) {
            layout(legacy);
            transfers();
        }
        layout(true);
        readableNames = false;
        transfers();
        layout(true);
        for(auto& descriptor : descriptors) descriptor.iInterface = 0;
        transfers();

        unsigned char data[16]{};
        layout(true);
        names[1] = "Unrelated debug function";
        require(usbPlatformGateRead("1.10", data, sizeof(data), 10) != 0, "Legacy numbering overrode a different interface name");
        layout();
        endpoints[1][1].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
        require(usbPlatformGateRead("1.10", data, sizeof(data), 10) != 0, "Gate accepted an interface without bulk OUT");
        layout();
        shortTransfer = true;
        require(usbPlatformGateRead("1.10", data, sizeof(data), 10) == LIBUSB_ERROR_IO, "Short Gate read reported success");
        require(usbPlatformGateWrite("1.10", data, sizeof(data), 10) == LIBUSB_ERROR_IO, "Short Gate write reported success");
        layout();
        failOpen = true;
        require(usbPlatformGateRead("1.10", data, sizeof(data), 10) != 0, "Failed open reported success");
        for(auto reply : {DiscoveryReply::Valid, DiscoveryReply::ShortRequest, DiscoveryReply::ShortHeader,
                          DiscoveryReply::ShortPayload, DiscoveryReply::Error, DiscoveryReply::ApplicationError, DiscoveryReply::Empty,
                          DiscoveryReply::Truncated, DiscoveryReply::Oversized}) {
            layout();
            discoveryReply = reply;
            require(discovered() == (reply == DiscoveryReply::Valid ? 1u : 0u), "Discovery accepted an incomplete or invalid response");
            require(resets == (reply == DiscoveryReply::Valid || reply == DiscoveryReply::ApplicationError ? 0 : 1), "Wrong discovery recovery");
        }
        for(bool legacy : {false, true}) {
            layout(legacy);
            XLinkGateTransaction* transaction = nullptr;
            require(usbPlatformGateBegin("1.10", &transaction) == 0, "Transaction open failed");
            require(usbPlatformGateTransactionWrite(transaction, data, sizeof(data), 10) == 0, "Transaction write failed");
            XLinkGateTransaction* second = nullptr;
            require(usbPlatformGateBegin("1.10", &second) == LIBUSB_ERROR_BUSY && second == nullptr, "Another client interleaved an exchange");
            require(discovered() == 0, "Discovery entered an active transaction");
            require(resets == 0, "Failed claim reset another client's transaction");
            require(usbPlatformGateTransactionRead(transaction, data, sizeof(data), 10) == 0 && data[0] == 0x5a, "Transaction lost its response");
            require(usbPlatformGateEnd(transaction, XLINK_GATE_COMPLETE) == 0 && resets == 0, "Complete exchange reset endpoints");
            require(usbPlatformGateBegin("1.10", &second) == 0, "Completed transaction retained claim");
            require(usbPlatformGateEnd(second, XLINK_GATE_ABORTED) == 0 && resets == 0, "Unused transaction reset endpoints");
        }
        layout();
        XLinkGateTransaction* transaction = nullptr;
        require(usbPlatformGateBegin("1.10", &transaction) == 0, "Failed to open aborted exchange");
        require(usbPlatformGateTransactionWrite(transaction, data, sizeof(data), 10) == 0, "Failed to send abandoned request");
        require(usbPlatformGateEnd(transaction, XLINK_GATE_ABORTED) == 0 && resets == 1, "Abandoned exchange did not recover");
        layout();
        require(usbPlatformGateBegin("1.10", &transaction) == 0, "Failed to open transfer failure exchange");
        transferError = LIBUSB_ERROR_TIMEOUT;
        resetError = LIBUSB_ERROR_NOT_SUPPORTED;
        require(usbPlatformGateTransactionRead(transaction, data, sizeof(data), 10) == LIBUSB_ERROR_TIMEOUT, "Transfer error changed");
        require(usbPlatformGateEnd(transaction, XLINK_GATE_ABORTED) == LIBUSB_ERROR_TIMEOUT && resets == 1, "Reset failure hid the transfer error");
        layout();
        claimError = LIBUSB_ERROR_BUSY;
        require(usbPlatformGateBegin("1.10", &transaction) == LIBUSB_ERROR_BUSY && transaction == nullptr && resets == 0, "Failed claim leaked or reset");
        layout();
        failOpen = true;
        require(usbPlatformGateBegin("1.10", &transaction) != 0 && transaction == nullptr && resets == 0, "Failed open leaked or reset");
        for(auto reply : {SyncReply::Echo, SyncReply::LateReply, SyncReply::SplitEcho, SyncReply::Retry}) {
            layout(false, reply);
            require(usbPlatformGateBegin("1.10", &transaction) == 0, "SYNC failed to reach the serial barrier");
            require(userWrites == 0, "SYNC replayed a user request");
            require(syncRequests.size() == (reply == SyncReply::Retry ? 2u : 1u), "SYNC used an unexpected attempt count");
            require(resets == (reply == SyncReply::Retry ? 1 : 0), "SYNC reset after a successful short read");
            require(usbPlatformGateTransactionWrite(transaction, data, sizeof(data), 10) == 0 && userWrites == 1, "Synchronized transaction did not send its request");
            require(usbPlatformGateEnd(transaction, XLINK_GATE_COMPLETE) == 0, "Synchronized transaction did not close");
        }
        for(auto reply : {SyncReply::MissingEcho, SyncReply::Timeout, SyncReply::Flood, SyncReply::TrailingData, SyncReply::ShortWrite, SyncReply::ShortHeader}) {
            layout(false, reply);
            require(usbPlatformGateBegin("1.10", &transaction) != 0 && transaction == nullptr, "Incomplete SYNC permitted a user RPC");
            require(userWrites == 0, "Failed SYNC sent a user request");
            const bool timedOut = reply == SyncReply::MissingEcho || reply == SyncReply::Timeout;
            require(syncRequests.size() == (timedOut ? 4u : reply == SyncReply::ShortHeader ? 0u : 1u), "SYNC retry bound failed");
            require(resets == (timedOut ? 4 : 0), "SYNC reset without an input error");
            if(reply == SyncReply::Flood) require(syncReadCount == 256, "SYNC discard bound failed");
        }
        layout(false, SyncReply::Timeout);
        resetError = LIBUSB_ERROR_NOT_SUPPORTED;
        require(usbPlatformGateBegin("1.10", &transaction) == LIBUSB_ERROR_TIMEOUT && transaction == nullptr, "SYNC reset failure hid the input error");
        require(syncRequests.size() == 1 && resets == 1 && userWrites == 0, "SYNC continued after failed recovery");
        layout();
        syncReply = SyncReply::Echo;
        require(usbPlatformGateBegin("1.10", &transaction) == 0, "Legacy transaction failed");
        require(usbPlatformGateTransactionWrite(transaction, data, sizeof(data), 10) == 0 && userWrites == 1 && syncRequests.empty(), "Protocol zero acquired a SYNC requirement");
        require(usbPlatformGateEnd(transaction, XLINK_GATE_COMPLETE) == 0, "Legacy transaction close failed");
        layout(false, SyncReply::Echo);
        discoveryReply = DiscoveryReply::Valid;
        require(discovered() == 1 && syncRequests.size() == 1 && userWrites == 1, "Discovery crossed the wrong barrier");
        layout(false, SyncReply::Timeout);
        require(discovered() == 0, "Discovery ignored a missing SYNC");
        require(userWrites == 0 && syncRequests.size() == 4, "Discovery sent a request before synchronization");
        layout();
        descriptors[1].bInterfaceProtocol = 1;
        require(usbPlatformGateWrite("1.10", data, sizeof(data), 10) == 0, "Legacy chunk write failed on a capable interface");
        require(usbPlatformGateRead("1.10", data, sizeof(data), 10) == 0 && data[0] == 0x5a && syncRequests.empty(), "Legacy chunks synchronized between header and body");
        layout();
        descriptors[1].bInterfaceProtocol = 2;
        require(usbPlatformGateBegin("1.10", &transaction) == LIBUSB_ERROR_NOT_SUPPORTED && transaction == nullptr && resets == 0, "Unknown Gate protocol was accepted");
        checkReleased();
        std::cout << "USB named interfaces, legacy layouts, endpoint selection and error cleanup passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
