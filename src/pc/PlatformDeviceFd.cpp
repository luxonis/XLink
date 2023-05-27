#include "PlatformDeviceFd.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

static std::mutex mutex;
static std::unordered_map<std::uintptr_t, void*> map;
static std::uintptr_t uniqueFdKey{0x55};

// Returns the mapped fd value of the element with key equivalent to fdKeyRaw
// Non-zero return value indicates failure
int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd) noexcept {
    if(fd == nullptr) return -1;
    try {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = map.find(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
        if(result == map.end())
            return 1;
        *fd = result->second;
        return 0;
    } catch(std::exception&) {
        return -1;
    }
}

// Inserts a copy of value fd into an associative container with key fdKeyRaw
// nullptr return value indicates failure
void* createPlatformDeviceFdKey(void* fd) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        std::uintptr_t fdKey = uniqueFdKey++; // Get a unique key
        map[fdKey] = fd;
        return reinterpret_cast<void*>(fdKey);
    } catch(std::exception&) {
        return nullptr;
    }
}

// Removes the element (if one exists) with the key equivalent to fdKeyRaw
// Non-zero return value indicates failure
int destroyPlatformDeviceFdKey(void* fdKeyRaw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        return !map.erase(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
    } catch(std::exception&) {
        return -1;
    }
}

// Extracts (finds and removes) the element (if one exists) with the key equivalent to fdKeyRaw
// nullptr return value indicates failure
// This atomic operation prevents a set of race conditions of two threads each get() and/or destroy()
// keys in unpredictable orders.
void* extractPlatformDeviceFdKey(void* fdKeyRaw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = map.find(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
        if(result == map.end())
            return nullptr;
        const auto fd = result->second;
        map.erase(result);
        return fd;
    } catch(std::exception&) {
        return nullptr;
    }
}

// TODO consider storing the device_handle in the fdKey store with std::map<device_handle> so that
// it can automatically handle refcounting, interfaces, and closing
