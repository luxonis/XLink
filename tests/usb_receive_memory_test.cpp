#include "usb_host_memory.h"
#include <XLink/XLinkPlatform.h>
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t frameSize = 4 * 1024 * 1024;
struct Allocation { libusb_device_handle* handle; size_t size; };
std::map<unsigned char*, Allocation> allocations;
std::set<libusb_device_handle*> closed;
unsigned mapped = 0, unmapped = 0, customAllocations = 0, customFrees = 0;
bool failMapping = false, failCustom = false;
bool failBudgetRead = false;
std::string budget = "128\n";

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

void* customAllocate(uint32_t size, uint32_t alignment, void*) {
    ++customAllocations;
    if(failCustom) return nullptr;
    void* data = nullptr;
    require(posix_memalign(&data, alignment, size) == 0, "Cannot allocate test storage");
    return data;
}

void customFree(void* data, uint32_t, uint32_t, void*) {
    ++customFrees;
    std::free(data);
}

xLinkDeviceHandle_t connection(uintptr_t identity, XLinkProtocol_t protocol) {
    xLinkDeviceHandle_t result{};
    result.protocol = protocol;
    result.xLinkFD = reinterpret_cast<void*>(identity);
    require(usbRegisterReadBufferHandle(result.xLinkFD, reinterpret_cast<void*>(identity + 0x1000)), "Cannot register USB connection");
    return result;
}

unsigned char* receive(xLinkDeviceHandle_t& device, uint32_t size = frameSize) {
    return static_cast<unsigned char*>(XLinkPlatformAllocateReadData(&device, size, 64));
}

void lifetime() {
    auto first = connection(1, X_LINK_USB_EP);
    auto* data = receive(first);
    require(data && allocations.count(data) == 1, "USB receive did not use mapped storage");
    std::memset(data, 0x5A, frameSize);
    XLinkDeallocateMoveData(data, frameSize);
    auto* reused = receive(first, frameSize / 2);
    require(reused == data && mapped == 1 && reused[123] == 0x5A, "Returned storage was not reused without copying");

    require(usbCloseReadBufferHandle(first.xLinkFD), "Cannot close first connection");
    require(closed.count(reinterpret_cast<libusb_device_handle*>(0x1001)) == 0, "Connection close invalidated a held frame");
    require(reused[frameSize / 2 - 1] == 0x5A, "Held frame changed after connection close");
    require(usbAllocateReadBuffer(first.xLinkFD, frameSize, 64) == nullptr, "Closed connection allocated mapped storage");

    auto next = connection(2, X_LINK_USB_VSC);
    auto* nextData = receive(next);
    require(nextData && nextData != reused && allocations.at(nextData).handle != allocations.at(reused).handle,
            "Reconnect reused the old connection's held allocation");
    std::memset(nextData, 0xA5, frameSize);
    require(XLinkSetDataAllocator(customAllocate, customFree, nullptr) == X_LINK_SUCCESS, "Cannot select custom allocator");
    XLinkDeallocateMoveData(reused, 17);
    require(customFrees == 0 && closed.count(reinterpret_cast<libusb_device_handle*>(0x1001)) == 1,
            "Mapped frame lost its allocation owner or original capacity");
    require(nextData[31] == 0xA5, "Freeing old frame changed reconnected storage");
    XLinkDeallocateMoveData(nextData, frameSize);
    require(usbCloseReadBufferHandle(next.xLinkFD), "Cannot close next connection");
    require(allocations.empty() && mapped == unmapped && closed.size() == 2, "Receive handles or mappings leaked");
    require(XLinkSetDataAllocator(nullptr, nullptr, nullptr) == X_LINK_SUCCESS, "Cannot restore allocator");
}

void fallbackAndPriority() {
    auto device = connection(3, X_LINK_USB_EP);
    const auto previous = mapped;
    failMapping = true;
    auto* ordinary = receive(device);
    require(ordinary && allocations.count(ordinary) == 0, "Mapping failure did not fall back to ordinary storage");
    ordinary[27] = 0x71;
    require(ordinary[27] == 0x71, "Fallback storage is not writable");
    XLinkDeallocateMoveData(ordinary, frameSize);
    failMapping = false;
    auto* small = receive(device, 128);
    require(small && mapped == previous, "Small protocol payload allocated mapped storage");
    XLinkDeallocateMoveData(small, 128);
    device.protocol = X_LINK_TCP_IP;
    ordinary = receive(device);
    require(ordinary && mapped == previous, "TCP receive used USB storage");
    XLinkDeallocateMoveData(ordinary, frameSize);
    device.protocol = X_LINK_USB_EP;

    require(XLinkSetDataAllocator(customAllocate, customFree, nullptr) == X_LINK_SUCCESS, "Cannot select custom allocator");
    auto* custom = receive(device);
    require(custom && customAllocations == 1 && mapped == previous, "USB mapped allocation bypassed custom allocator");
    XLinkDeallocateMoveData(custom, frameSize);
    require(customFrees == 1, "Custom receive storage used the wrong release");
    failCustom = true;
    require(receive(device) == nullptr && mapped == previous, "Custom allocator failure silently fell back");
    failCustom = false;
    require(XLinkSetDataAllocator(nullptr, nullptr, nullptr) == X_LINK_SUCCESS, "Cannot restore default allocator");
    auto* mappedData = receive(device);
    require(mappedData && allocations.count(mappedData) == 1, "A transient mapping failure disabled later mapped receives");
    XLinkDeallocateMoveData(mappedData, frameSize);
    require(usbCloseReadBufferHandle(device.xLinkFD), "Cannot close fallback connection");
}

void boundedPool() {
    auto device = connection(4, X_LINK_USB_VSC);
    const uint32_t large = 32 * 1024 * 1024;
    auto* first = receive(device, large);
    auto* second = receive(device, large);
    auto* third = receive(device, large);
    require(first && second && third && allocations.count(first) == 1 && allocations.count(second) == 1
            && allocations.count(third) == 0, "Mapped pool exceeded 64 MiB or rejected fallback");
    XLinkDeallocateMoveData(third, large);
    XLinkDeallocateMoveData(first, large);
    auto* reused = receive(device, frameSize);
    require(reused == first, "Pool did not reuse sufficient capacity");
    XLinkDeallocateMoveData(reused, frameSize);
    XLinkDeallocateMoveData(second, large);
    auto* resized = receive(device, 48 * 1024 * 1024);
    require(resized && allocations.size() == 1 && allocations.at(resized).size == 48 * 1024 * 1024,
            "Incompatible idle mappings prevented bounded pool resize");
    XLinkDeallocateMoveData(resized, 48 * 1024 * 1024);
    require(usbCloseReadBufferHandle(device.xLinkFD), "Cannot close bounded pool");
    require(allocations.empty() && mapped == unmapped, "Closing pool did not free idle mappings");
}

void sharedBudget() {
    budget = "16\n";
    auto first = connection(5, X_LINK_USB_EP);
    auto second = connection(6, X_LINK_USB_VSC);
    const uint32_t size = 4177920 + 512;
    auto* held = receive(first, size);
    auto* other = receive(second, size);
    auto* overflow = receive(second, size);
    require(allocations.count(held) == 1 && allocations.count(other) == 1 && allocations.count(overflow) == 0,
            "Connections did not share half the kernel mapping budget");
    XLinkDeallocateMoveData(overflow, size);
    require(usbCloseReadBufferHandle(first.xLinkFD), "Cannot close held-budget connection");
    overflow = receive(second, size);
    require(allocations.count(overflow) == 0, "Logical close released the budget of a held mapping");
    XLinkDeallocateMoveData(overflow, size);
    XLinkDeallocateMoveData(held, size);
    auto* available = receive(second, size);
    require(allocations.count(available) == 1, "Actual unmap did not release shared budget");
    XLinkDeallocateMoveData(other, size);
    XLinkDeallocateMoveData(available, size);
    require(usbCloseReadBufferHandle(second.xLinkFD), "Cannot close shared-budget connection");
}

void budgetSettings() {
    uintptr_t key = 7;
    for(const auto& limit : std::vector<std::pair<std::string, unsigned>>{{"64\n", 1}, {"128\n", 3}, {"0\n", 3}, {"invalid\n", 0}, {"16 invalid\n", 0}, {"-1\n", 0}, {"4294967296\n", 0}}) {
        budget = limit.first;
        auto device = connection(key++, X_LINK_USB_EP);
        const uint32_t size = 18 * 1024 * 1024;
        std::vector<unsigned char*> frames;
        for(unsigned i = 0; i < 4; ++i) {
            auto* data = receive(device, size);
            require(data && (allocations.count(data) == 1) == (i < limit.second), "Kernel mapping budget was not respected");
            frames.push_back(data);
        }
        for(auto* data : frames) XLinkDeallocateMoveData(data, size);
        require(usbCloseReadBufferHandle(device.xLinkFD), "Cannot close budget-setting connection");
    }
    failBudgetRead = true;
    auto device = connection(key++, X_LINK_USB_EP);
    auto* data = receive(device);
    require(data && allocations.count(data) == 0, "Unreadable kernel budget did not disable mapping");
    XLinkDeallocateMoveData(data, frameSize);
    require(usbCloseReadBufferHandle(device.xLinkFD), "Cannot close unreadable-budget connection");
    failBudgetRead = false;
}
}

extern "C" {
FILE* __wrap_fopen(const char* path, const char*) {
    require(std::strcmp(path, "/sys/module/usbcore/parameters/usbfs_memory_mb") == 0, "Unexpected budget path");
    if(failBudgetRead) return nullptr;
    auto* file = std::tmpfile();
    require(file != nullptr, "Cannot create budget fixture");
    std::fputs(budget.c_str(), file);
    std::rewind(file);
    return file;
}

unsigned char* __wrap_libusb_dev_mem_alloc(libusb_device_handle* handle, size_t size) {
    require(closed.count(handle) == 0, "Mapped storage uses a closed handle");
    if(failMapping) return nullptr;
    void* data = nullptr;
    require(posix_memalign(&data, 4096, size) == 0, "Cannot allocate mock USB memory");
    auto* bytes = static_cast<unsigned char*>(data);
    allocations.emplace(bytes, Allocation{handle, size});
    ++mapped;
    return bytes;
}

int __wrap_libusb_dev_mem_free(libusb_device_handle* handle, unsigned char* data, size_t size) {
    const auto found = allocations.find(data);
    require(found != allocations.end() && found->second.handle == handle && found->second.size == size,
            "Mapped storage was freed with the wrong handle, address, or capacity");
    require(closed.count(handle) == 0, "Mapped frame freed after its USB handle");
    allocations.erase(found);
    std::free(data);
    ++unmapped;
    return 0;
}

void __wrap_libusb_close(libusb_device_handle* handle) {
    for(const auto& allocation : allocations) require(allocation.second.handle != handle, "USB handle closed before held mappings");
    require(closed.insert(handle).second, "USB handle closed twice");
}
}

int main() {
    try {
        lifetime();
        fallbackAndPriority();
        boundedPool();
        sharedBudget();
        budgetSettings();
        require(allocations.empty() && mapped == unmapped && closed.size() == 14, "USB receive storage leaked");
        std::cout << "Mapped USB receive reuse, retained-frame lifetime, allocator priority, fallback, and pool bounds passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
