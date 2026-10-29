#include "XLink.h"

#include <mutex>
#include <functional>
#include <unordered_map>


static std::mutex mtx;
static uint16_t uniqueId{0};
static std::unordered_map<int, std::function<void(linkId_t)>> callbacks;

extern "C" {

int XLinkAddLinkDownCb(void (*cb)(linkId_t)) {
    std::unique_lock<std::mutex> l(mtx);

    uint16_t cbId = uniqueId++;
    if(callbacks.count(cbId)) {
        return -1;
    }
    callbacks[cbId] = cb;

    return cbId;
}

int XLinkRemoveLinkDownCb(int cbId) {
    std::unique_lock<std::mutex> l(mtx);
    if(callbacks.count(cbId)) {
        callbacks.erase(cbId);
    } else {
        return -1;
    }

    return 0;
}

void XLinkPlatformLinkDownNotify(linkId_t linkId) {
    std::unique_lock<std::mutex> l(mtx);
    for(const auto& kv : callbacks) {
        kv.second(linkId);
    }
}

}