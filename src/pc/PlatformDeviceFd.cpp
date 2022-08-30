#include "PlatformDeviceFd.h"

#include <unordered_map>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <memory>


class PlatformDeviceFd {

    mutable std::mutex mutex;
    std::unique_ptr<std::unordered_map<std::uintptr_t, void*>> up_map;
    std::unordered_map<std::uintptr_t, void*>& map;
    std::uintptr_t uniqueFdKey{0x55};

    // private constructor
    PlatformDeviceFd() : up_map(new std::unordered_map<std::uintptr_t, void*>), map(*up_map) {}
    ~PlatformDeviceFd() {
        std::unique_lock<std::mutex> lock(mutex);
        up_map = nullptr;
    }

public:
    static PlatformDeviceFd& getInstance() {
        static PlatformDeviceFd instance;  // Guaranteed to be destroyed, instantiated on first use.
        return instance;
    }
    PlatformDeviceFd(PlatformDeviceFd const&) = delete;
    void operator=(PlatformDeviceFd const&) = delete;

    int getFdFromKey(void* fdKeyRaw, void** fd) {
        if(fd == nullptr) return -1;
        std::unique_lock<std::mutex> lock(mutex);

        std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
        if(map.count(fdKey) > 0){
            *fd = map.at(fdKey);
            return 0;
        } else {
            return 1;
        }
    }

    void* createFdKey(void* fd) {
        std::unique_lock<std::mutex> lock(mutex);

        // Get uniqueFdKey
        std::uintptr_t fdKey = uniqueFdKey++;
        map[fdKey] = fd;
        return reinterpret_cast<void*>(fdKey);
    }

    int destroyFdKey(void* fdKeyRaw) {
        std::unique_lock<std::mutex> lock(mutex);

        std::uintptr_t fdKey = reinterpret_cast<std::uintptr_t>(fdKeyRaw);
        if(map.count(fdKey) > 0){
            map.erase(fdKey);
            return 0;
        } else {
            return -1;
        }
    }
};


int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd){
    return PlatformDeviceFd::getInstance().getFdFromKey(fdKeyRaw, fd);
}

void* createPlatformDeviceFdKey(void* fd){
    return PlatformDeviceFd::getInstance().createFdKey(fd);
}

int destroyPlatformDeviceFdKey(void* fdKeyRaw){
   return PlatformDeviceFd::getInstance().destroyFdKey(fdKeyRaw);
}
