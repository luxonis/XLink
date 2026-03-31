#include "XLink.h"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex& getCallbacksMutex() {
    static auto* mtx = new std::mutex();
    return *mtx;
}

uint16_t& getUniqueId() {
    static auto* uniqueId = new uint16_t{0};
    return *uniqueId;
}

std::unordered_map<int, std::function<void(linkId_t)>>& getCallbacks() {
    static auto* callbacks = new std::unordered_map<int, std::function<void(linkId_t)>>();
    return *callbacks;
}

}  // namespace

extern "C" {

int XLinkAddLinkDownCb(void (*cb)(linkId_t)) {
    auto& callbacks = getCallbacks();
    auto& uniqueId = getUniqueId();
    std::unique_lock<std::mutex> l(getCallbacksMutex());

    uint16_t cbId = uniqueId++;
    if(callbacks.count(cbId)) {
        return -1;
    }
    callbacks[cbId] = cb;

    return cbId;
}

int XLinkRemoveLinkDownCb(int cbId) {
    auto& callbacks = getCallbacks();
    std::unique_lock<std::mutex> l(getCallbacksMutex());
    if(callbacks.count(cbId)) {
        callbacks.erase(cbId);
    } else {
        return -1;
    }

    return 0;
}

void XLinkPlatformLinkDownNotify(linkId_t linkId) {
    auto& callbacks = getCallbacks();
    std::unique_lock<std::mutex> l(getCallbacksMutex());
    for(const auto& kv : callbacks) {
        kv.second(linkId);
    }
}

}