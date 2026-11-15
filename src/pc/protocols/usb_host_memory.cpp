#include "usb_host_memory.h"

#if defined(__linux__) && defined(XLINK_ENABLE_LIBUSB)
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {
constexpr size_t minimumMappedSize = 64 * 1024;
constexpr size_t maximumMappedBytes = 64 * 1024 * 1024;
std::atomic<size_t> totalMappedBytes{0};

size_t mappedByteLimit() {
    auto* file = std::fopen("/sys/module/usbcore/parameters/usbfs_memory_mb", "r");
    if(!file) return 0;
    unsigned long long mib = 0;
    char trailing;
    const int fields = std::fscanf(file, "%llu %c", &mib, &trailing);
    std::fclose(file);
    if(fields != 1 || mib > UINT32_MAX) return 0;
    if(!mib) return maximumMappedBytes;
    // USBFS charges ordinary transfer data and URB metadata in the same budget.
    return static_cast<size_t>(std::min<unsigned long long>(mib * 1024 * 1024 / 2, SIZE_MAX));
}

bool reserveMappedBytes(size_t capacity) {
    const auto limit = mappedByteLimit();
    auto reserved = totalMappedBytes.load();
    do {
        if(capacity > limit || reserved > limit - capacity) return false;
    } while(!totalMappedBytes.compare_exchange_weak(reserved, reserved + capacity));
    return true;
}

struct Mapping {
    unsigned char* data;
    size_t capacity;
};

struct ReceivePool {
    libusb_device_handle* handle = nullptr;
    std::mutex mutex;
    bool open = true;
    size_t mappedBytes = 0;
    std::vector<Mapping> idle;

    ~ReceivePool() {
        discardIdle();
        if(handle) libusb_close(handle);
    }

    void unmap(Mapping mapping) {
        if(libusb_dev_mem_free(handle, mapping.data, mapping.capacity) == 0) {
            mappedBytes -= mapping.capacity;
            totalMappedBytes.fetch_sub(mapping.capacity);
        }
    }

    void discardIdle() {
        for(const auto& mapping : idle) unmap(mapping);
        idle.clear();
    }

    Mapping acquire(size_t size, uint32_t alignment) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!open) return {nullptr, 0};
        auto best = idle.end();
        for(auto current = idle.begin(); current != idle.end(); ++current) {
            if(current->capacity >= size && reinterpret_cast<uintptr_t>(current->data) % alignment == 0
               && (best == idle.end() || current->capacity < best->capacity)) {
                best = current;
            }
        }
        if(best != idle.end()) {
            const auto mapping = *best;
            idle.erase(best);
            return mapping;
        }

        discardIdle();
        const auto pageSize = sysconf(_SC_PAGESIZE);
        if(pageSize <= 0) return {nullptr, 0};
        const size_t capacity = (size + pageSize - 1) / pageSize * pageSize;
        if(capacity > maximumMappedBytes - mappedBytes) return {nullptr, 0};
        if(!reserveMappedBytes(capacity)) return {nullptr, 0};
        auto* data = libusb_dev_mem_alloc(handle, capacity);
        if(!data) {
            totalMappedBytes.fetch_sub(capacity);
            return {nullptr, 0};
        }
        mappedBytes += capacity;
        if(reinterpret_cast<uintptr_t>(data) % alignment != 0) {
            unmap({data, capacity});
            return {nullptr, 0};
        }
        return {data, capacity};
    }

    void release(Mapping mapping) {
        std::lock_guard<std::mutex> lock(mutex);
        if(open) {
            try {
                idle.push_back(mapping);
                return;
            } catch(...) {
            }
        }
        unmap(mapping);
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
        discardIdle();
    }
};

struct ReceiveBuffer {
    std::shared_ptr<ReceivePool> pool;
    Mapping mapping;

    ~ReceiveBuffer() { if(mapping.data) pool->release(mapping); }
};

struct ReceiveStorage {
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<ReceivePool>> connections;
    std::unordered_map<void*, std::unique_ptr<ReceiveBuffer>> buffers;
};

ReceiveStorage* storage() {
    // Frames can be destroyed by application globals after XLink's globals.
    static auto* state = new(std::nothrow) ReceiveStorage;
    return state;
}
}

bool usbRegisterReadBufferHandle(void* key, void* handle) {
    if(!key || !handle) return false;
    try {
        auto pool = std::make_shared<ReceivePool>();
        auto* state = storage();
        if(!state) return false;
        std::lock_guard<std::mutex> lock(state->mutex);
        if(!state->connections.emplace(key, pool).second) return false;
        pool->handle = static_cast<libusb_device_handle*>(handle);
        return true;
    } catch(...) {
        return false;
    }
}

bool usbCloseReadBufferHandle(void* key) {
    std::shared_ptr<ReceivePool> pool;
    auto* state = storage();
    if(!state) return false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto found = state->connections.find(key);
        if(found == state->connections.end()) return false;
        pool = std::move(found->second);
        state->connections.erase(found);
    }
    pool->close();
    return true;
}

void* usbAllocateReadBuffer(void* key, uint32_t size, uint32_t alignment) {
    if(size < minimumMappedSize || size > maximumMappedBytes || !alignment) return nullptr;
    try {
        auto* state = storage();
        if(!state) return nullptr;
        std::shared_ptr<ReceivePool> pool;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto found = state->connections.find(key);
            if(found == state->connections.end()) return nullptr;
            pool = found->second;
        }
        std::unique_ptr<ReceiveBuffer> buffer(new ReceiveBuffer{pool, {nullptr, 0}});
        buffer->mapping = pool->acquire(size, alignment);
        if(!buffer->mapping.data) return nullptr;
        auto* data = buffer->mapping.data;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->buffers.emplace(data, std::move(buffer)).second ? data : nullptr;
    } catch(...) {
        return nullptr;
    }
}

bool usbFreeReadBuffer(void* data) {
    std::unique_ptr<ReceiveBuffer> buffer;
    auto* state = storage();
    if(!state) return false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto found = state->buffers.find(data);
        if(found == state->buffers.end()) return false;
        buffer = std::move(found->second);
        state->buffers.erase(found);
    }
    return true;
}

#else
bool usbRegisterReadBufferHandle(void*, void*) { return false; }
bool usbCloseReadBufferHandle(void*) { return false; }
void* usbAllocateReadBuffer(void*, uint32_t, uint32_t) { return nullptr; }
bool usbFreeReadBuffer(void*) { return false; }
#endif
