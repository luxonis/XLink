#include <XLink/XLink.h>
#include <XLink/XLinkPlatform.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
struct Storage {
    alignas(64) std::array<uint8_t, 128> bytes{};
    unsigned allocated = 0;
    unsigned freed = 0;
};
void require(bool ok, const char* message) {
    if(!ok) throw std::runtime_error(message);
}
void* allocate(uint32_t size, uint32_t alignment, void* context) {
    auto& storage = *static_cast<Storage*>(context);
    if(size > storage.bytes.size() || alignment > 64) return nullptr;
    ++storage.allocated;
    return storage.bytes.data();
}
void deallocate(void* data, uint32_t size, uint32_t alignment, void* context) {
    auto& storage = *static_cast<Storage*>(context);
    require(data == storage.bytes.data() && size == 128 && alignment == 64, "Release lost allocation identity");
    ++storage.freed;
}
}
int main() {
    try {
        Storage storage;
        require(XLinkSetDataAllocator(allocate, nullptr, &storage) == X_LINK_ERROR, "Accepted unpaired callbacks");
        require(XLinkSetDataAllocator(allocate, deallocate, &storage) == X_LINK_SUCCESS, "Could not install allocator");
        auto* data = static_cast<uint8_t*>(XLinkPlatformAllocateData(128, 64));
        require(data == storage.bytes.data() && storage.allocated == 1, "Receive allocation did not use custom storage");
        data[17] = 0xAB;
        require(storage.bytes[17] == 0xAB, "Receive bytes do not alias custom storage");
        require(XLinkPlatformAllocateData(256, 64) == nullptr, "Allocation failure was silently replaced");
        XLinkPlatformDeallocateData(data, 128, 64);
        require(storage.freed == 1, "Custom storage was not released");
        XLinkPlatformDeallocateData(nullptr, 128, 64);
        require(storage.freed == 1, "Null release called allocator");
        require(XLinkSetDataAllocator(nullptr, nullptr, nullptr) == X_LINK_SUCCESS, "Could not restore default");
        auto* ordinary = XLinkPlatformAllocateData(128, 64);
        require(ordinary && reinterpret_cast<uintptr_t>(ordinary) % 64 == 0, "Default alignment changed");
        XLinkPlatformDeallocateData(ordinary, 128, 64);
        require(XLinkSetDataAllocator(allocate, deallocate, &storage) == X_LINK_SUCCESS, "Could not reinstall allocator");
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "Initialization failed");
        require(XLinkSetDataAllocator(nullptr, nullptr, nullptr) == X_LINK_ALREADY_OPEN, "Changed allocator after initialization");
        data = static_cast<uint8_t*>(XLinkPlatformAllocateData(128, 64));
        require(data == storage.bytes.data(), "Rejected change replaced allocator");
        XLinkPlatformDeallocateData(data, 128, 64);
        require(storage.allocated == 2 && storage.freed == 2, "Allocator lifetime mismatch");
        std::cout << "Receive allocator tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
