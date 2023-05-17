#include "PlatformDeviceFd.h"

#include <unordered_map>
#include <atomic>
#include <mutex>
#include <cstdint>

static std::mutex mutex;
static std::unordered_map<std::uintptr_t, void*> map;
static std::uintptr_t uniqueFdKey{0x55};

int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd){
    if(fd == nullptr) return -1;
    std::lock_guard<std::mutex> lock(mutex);

    std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
    if(map.count(fdKey) > 0){
        *fd = map.at(fdKey);
        return 0;
    } else {
        return 1;
    }
}

void* createPlatformDeviceFdKey(void* fd){
    std::lock_guard<std::mutex> lock(mutex);

    // Get uniqueFdKey
    std::uintptr_t fdKey = uniqueFdKey++;
    map[fdKey] = fd;
    return reinterpret_cast<void*>(fdKey);
}

int destroyPlatformDeviceFdKey(void* fdKeyRaw){
    std::lock_guard<std::mutex> lock(mutex);

    std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
    if(map.count(fdKey) > 0){
        map.erase(fdKey);
        return 0;
    } else {
        return -1;
    }
}
