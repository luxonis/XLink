#include "XLinkPrivateFields.h"

#include <condition_variable>
#include <mutex>

namespace {

struct XLinkSessionLifecycle {
    std::mutex mutex;
    std::condition_variable stoppedCv;
    XLinkSessionState_t state = XLINK_SESSION_STARTING;
    bool callbackDelivered = false;
    XLinkLinkDownReason_t reason = X_LINK_LINK_DOWN_UNKNOWN;
};

XLinkSessionLifecycle* getLifecycle(const XLinkSession_t* session) {
    return session == nullptr ? nullptr : static_cast<XLinkSessionLifecycle*>(session->lifecycle);
}

}  // namespace

extern "C" {

int XLinkSessionLifecycleInit(XLinkSession_t* session) {
    if (session == nullptr) {
        return 1;
    }
    try {
        session->lifecycle = new XLinkSessionLifecycle();
    } catch (...) {
        session->lifecycle = nullptr;
        return 1;
    }
    return 0;
}

void XLinkSessionLifecycleDestroy(XLinkSession_t* session) {
    if (session == nullptr) {
        return;
    }
    delete getLifecycle(session);
    session->lifecycle = nullptr;
}

void XLinkSessionLifecycleMarkRunning(XLinkSession_t* session) {
    auto* lifecycle = getLifecycle(session);
    if (lifecycle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(lifecycle->mutex);
    lifecycle->state = XLINK_SESSION_RUNNING;
}

XLinkSessionState_t XLinkSessionLifecycleGetState(const XLinkSession_t* session) {
    auto* lifecycle = getLifecycle(session);
    if (lifecycle == nullptr) {
        return XLINK_SESSION_STOPPED;
    }
    std::lock_guard<std::mutex> lock(lifecycle->mutex);
    return lifecycle->state;
}

int XLinkSessionLifecycleBeginStop(XLinkSession_t* session, XLinkLinkDownReason_t reason) {
    auto* lifecycle = getLifecycle(session);
    if (lifecycle == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(lifecycle->mutex);
    if (lifecycle->reason == X_LINK_LINK_DOWN_UNKNOWN) {
        lifecycle->reason = reason;
    }
    if (lifecycle->state == XLINK_SESSION_STOPPING || lifecycle->state == XLINK_SESSION_STOPPED) {
        return 0;
    }
    lifecycle->state = XLINK_SESSION_STOPPING;
    return 1;
}

void XLinkSessionLifecycleMarkStopped(XLinkSession_t* session) {
    auto* lifecycle = getLifecycle(session);
    if (lifecycle == nullptr) {
        return;
    }

    XLinkLinkDownCallback_t callback = nullptr;
    void* callbackContext = nullptr;
    XLinkLinkDownReason_t reason = X_LINK_LINK_DOWN_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(lifecycle->mutex);
        lifecycle->state = XLINK_SESSION_STOPPED;
        lifecycle->stoppedCv.notify_all();
        if (!lifecycle->callbackDelivered && session->linkDownCallback != nullptr) {
            lifecycle->callbackDelivered = true;
            callback = session->linkDownCallback;
            callbackContext = session->linkDownCallbackContext;
            reason = lifecycle->reason;
        }
    }

    if (callback != nullptr) {
        callback(reason, callbackContext);
    }
}

int XLinkSessionLifecycleWaitStopped(XLinkSession_t* session) {
    auto* lifecycle = getLifecycle(session);
    if (lifecycle == nullptr) {
        return 1;
    }

    std::unique_lock<std::mutex> lock(lifecycle->mutex);
    lifecycle->stoppedCv.wait(lock, [&]() { return lifecycle->state == XLINK_SESSION_STOPPED; });
    return 0;
}

}  // extern "C"
