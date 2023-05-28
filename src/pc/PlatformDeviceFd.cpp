#define MVLOG_UNIT_NAME xLinkUsb

#include "PlatformDeviceFd.h"
#include "XLink/XLinkLog.h"

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
    if(fd == nullptr) {
        mvLog(MVLOG_ERROR, "getPlatformDeviceFdFromKey(%p) failed", fdKeyRaw);
        return -1;
    }
    try {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto result = map.find(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
        if(result == map.end()) {
            mvLog(MVLOG_ERROR, "getPlatformDeviceFdFromKey(%p) failed", fdKeyRaw);
            return 1;
        }
        *fd = result->second;
        //mvLog(MVLOG_DEBUG, "getPlatformDeviceFdFromKey(%p) result %p", fdKeyRaw, *fd);
        return 0;
    } catch(std::exception&) {
        mvLog(MVLOG_ERROR, "getPlatformDeviceFdFromKey(%p) failed", fdKeyRaw);
        return -1;
    }
}

// Inserts a copy of value fd into an associative container with key fdKeyRaw
// nullptr return value indicates failure
void* createPlatformDeviceFdKey(void* fd) noexcept {
    try {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::uintptr_t fdKey = uniqueFdKey++; // Get a unique key
        map[fdKey] = fd;
        mvLog(MVLOG_DEBUG, "createPlatformDeviceFdKey(%p) result %p", fd, reinterpret_cast<void*>(fdKey));
        return reinterpret_cast<void*>(fdKey);
    } catch(std::exception&) {
        mvLog(MVLOG_ERROR, "createPlatformDeviceFdKey(%p) failed", fd);
        return nullptr;
    }
}

// Removes the element (if one exists) with the key equivalent to fdKeyRaw
// Non-zero return value indicates failure
int destroyPlatformDeviceFdKey(void* fdKeyRaw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = map.erase(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
        if (result == 0)
            mvLog(MVLOG_ERROR, "destroyPlatformDeviceFdKey(%p) failed", fdKeyRaw);
        else
            mvLog(MVLOG_DEBUG, "destroyPlatformDeviceFdKey(%p) success", fdKeyRaw);
        return !result;
    } catch(std::exception&) {
        mvLog(MVLOG_ERROR, "destroyPlatformDeviceFdKey(%p) failed", fdKeyRaw);
        return -1;
    }
}

// Extracts (finds and removes) the element (if one exists) with the key equivalent to fdKeyRaw
// nullptr return value indicates failure
// Only one mutex lock is taken compared to separate getPlatformDeviceFdFromKey() then destroyPlatformDeviceFdKey().
// Additionally, this atomic operation prevents a set of race conditions of two threads when
// each get() and/or destroy() keys in unpredictable orders.
void* extractPlatformDeviceFdKey(void* fdKeyRaw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = map.find(reinterpret_cast<std::uintptr_t>(fdKeyRaw));
        if(result == map.end()) {
            mvLog(MVLOG_ERROR, "extractPlatformDeviceFdKey(%p) failed", fdKeyRaw);
            return nullptr;
        }
        const auto fd = result->second;
        map.erase(result);
        mvLog(MVLOG_DEBUG, "extractPlatformDeviceFdKey(%p) success", fdKeyRaw);
        return fd;
    } catch(std::exception&) {
        mvLog(MVLOG_ERROR, "extractPlatformDeviceFdKey(%p) failed", fdKeyRaw);
        return nullptr;
    }
}

// TODO consider storing the device_handle in the fdKey store with std::map<device_handle> so that
// it can automatically handle refcounting, interfaces, and closing
