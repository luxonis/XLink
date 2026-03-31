#include "PlatformDeviceFd.h"

#include <unordered_map>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace {

std::mutex& getPlatformFdMutex() {
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<std::uintptr_t, void*>& getPlatformFdMap() {
    static auto* map = new std::unordered_map<std::uintptr_t, void*>();
    return *map;
}

std::atomic<std::uintptr_t>& getUniqueFdKey() {
    static auto* uniqueFdKey = new std::atomic<std::uintptr_t>(0x55);
    return *uniqueFdKey;
}

}  // namespace

int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd){
    if(fd == nullptr) return -1;
    auto& mutex = getPlatformFdMutex();
    auto& map = getPlatformFdMap();
    std::unique_lock<std::mutex> lock(mutex);

    std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
    if(map.count(fdKey) > 0){
        *fd = map.at(fdKey);
        return 0;
    } else {
        return 1;
    }
}

void* createPlatformDeviceFdKey(void* fd){
    auto& mutex = getPlatformFdMutex();
    auto& map = getPlatformFdMap();
    auto& uniqueFdKey = getUniqueFdKey();
    std::unique_lock<std::mutex> lock(mutex);

    // Get uniqueFdKey
    std::uintptr_t fdKey = uniqueFdKey++;
    map[fdKey] = fd;
    return reinterpret_cast<void*>(fdKey);
}

int destroyPlatformDeviceFdKey(void* fdKeyRaw){
    auto& mutex = getPlatformFdMutex();
    auto& map = getPlatformFdMap();
    std::unique_lock<std::mutex> lock(mutex);

    std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
    if(map.count(fdKey) > 0){
        map.erase(fdKey);
        return 0;
    } else {
        return -1;
    }
}
