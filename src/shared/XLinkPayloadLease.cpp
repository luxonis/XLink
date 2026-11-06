#include "XLinkPrivateDefines.h"

#include <atomic>
#include <vector>

struct XLinkPayloadLease {
    XLinkPayloadLease(void* owner, void (*release)(void*), const uint8_t* data, int size)
        : owner(owner), release(release) {
        if (size) trailer.assign(data, data + size);
    }

    std::atomic<unsigned> refs{1};
    void* owner;
    void (*release)(void*);
    std::vector<uint8_t> trailer;
};

XLinkPayloadLease* XLinkPayloadLeaseCreate(void* owner, void (*release)(void*),
                                           const uint8_t* trailer, int trailerSize) {
    try {
        return new XLinkPayloadLease(owner, release, trailer, trailerSize);
    } catch (...) {
        release(owner);
        return nullptr;
    }
}

const uint8_t* XLinkPayloadLeaseTrailer(XLinkPayloadLease* lease) {
    return lease->trailer.data();
}

void XLinkPayloadLeaseRetain(XLinkPayloadLease* lease) {
    if (lease) lease->refs.fetch_add(1, std::memory_order_relaxed);
}

void XLinkPayloadLeaseRelease(XLinkPayloadLease* lease) {
    if (lease && lease->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        lease->release(lease->owner);
        delete lease;
    }
}
