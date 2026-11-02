#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
#include "XLink/XLinkStream.h"
XLinkError_t XLinkGetFillLevel(streamId_t streamId, int isRemote, int* fillLevel);
void __real_XLinkStreamReset(streamDesc_t* stream);
int __real_sem_wait(sem_t* semaphore);
}

namespace {
std::atomic<bool> armed{false};
std::atomic<bool> retiring{false};
std::atomic<bool> waiterRegistered{false};
std::atomic<bool> pauseLookup{false};
std::atomic<bool> allowLookup{false};
thread_local bool lookupCaller = false;
streamDesc_t* target = nullptr;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
}

extern "C" void __wrap_XLinkStreamReset(streamDesc_t* stream) {
    if(stream == target && armed.exchange(false)) {
        retiring.store(true);
        while(!waiterRegistered.load()) std::this_thread::yield();
    }
    __real_XLinkStreamReset(stream);
}

extern "C" int __wrap_sem_wait(sem_t* semaphore) {
    if(lookupCaller && target && semaphore == &target->sem.psem) {
        waiterRegistered.store(true);
        while(pauseLookup.load() && !allowLookup.load()) std::this_thread::yield();
    }
    return __real_sem_wait(semaphore);
}

namespace {
void closeWithWaiter(bool server, bool closeResponse, bool byName) {
    auto& link = availableXLinks[0];
    int identity = 0;
    link.id = 0;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &identity;
    auto& stream = link.availableStreams[0];
    require(XLinkStreamInitialize(&stream, 5, "asset") == X_LINK_SUCCESS, "Cannot initialize stream");
    stream.writeSize = closeResponse ? 64 : 0;
    stream.readSize = closeResponse ? 0 : 1024;
    releaseStream(&stream);

    target = &stream;
    retiring.store(false);
    waiterRegistered.store(false);
    armed.store(true);
    bool staleRejected = false;
    std::thread lookup([&] {
        lookupCaller = true;
        while(!retiring.load()) std::this_thread::yield();
        if(byName) {
            auto* found = getStreamByName(&link, "asset");
            staleRejected = found == nullptr;
            releaseStream(found);
        } else {
            int fill = -1;
            staleRejected = XLinkGetFillLevel(5, 0, &fill) == X_LINK_ERROR;
        }
    });

    xLinkEvent_t event{};
    event.header.type = closeResponse ? XLINK_CLOSE_STREAM_RESP : XLINK_CLOSE_STREAM_REQ;
    event.header.flags.bitField.ack = closeResponse;
    event.header.streamId = 5;
    event.deviceHandle = link.deviceHandle;
    xLinkEvent_t response{};
    require(dispatcherRemoteEventGetResponse(&event, &response, server) == 0, "Close handler failed");
    lookup.join();
    require(staleRejected, "Lookup returned a retired stream");
    require(stream.id == INVALID_STREAM_ID, "Retirement did not clear stream identity");
    require(XLinkStreamInitialize(&stream, 17, "reused") == X_LINK_SUCCESS, "Cannot reuse retired slot");
    releaseStream(&stream);
    link.id = 0;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &identity;
    require(getStreamById(&identity, 5) == nullptr, "Old id resolves to reused slot");
    auto* reused = getStreamById(&identity, 17);
    require(reused == &stream, "Reused slot lost synchronization");
    XLinkStreamReset(reused);
    releaseStream(reused);
    target = nullptr;
}
void lookupAcrossLinkReuse(bool byName) {
    auto& link = availableXLinks[0];
    auto& stream = link.availableStreams[0];
    int oldIdentity = 0;
    int newIdentity = 1;
    link.id = 0;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &oldIdentity;
    require(XLinkStreamInitialize(&stream, 5, "asset") == X_LINK_SUCCESS, "Cannot initialize old stream");
    releaseStream(&stream);
    require(XLink_sem_init(&link.dispatcherClosedSem, 0, 0) == 0, "Cannot initialize old link completion");
    target = &stream;
    armed.store(false);
    waiterRegistered.store(false);
    allowLookup.store(false);
    pauseLookup.store(true);
    bool staleRejected = false;
    std::thread lookup([&] {
        lookupCaller = true;
        auto* found = byName ? getStreamByName(&link, "asset") : getStreamById(&oldIdentity, 5);
        staleRejected = found == nullptr;
        releaseStream(found);
    });
    while(!waiterRegistered.load()) std::this_thread::yield();
    dispatcherCloseLink(&oldIdentity, 1);
    link.id = 1;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &newIdentity;
    require(XLinkStreamInitialize(&stream, 5, "asset") == X_LINK_SUCCESS, "Cannot initialize reused link stream");
    releaseStream(&stream);
    allowLookup.store(true);
    lookup.join();
    pauseLookup.store(false);
    auto* current = getStreamById(&newIdentity, 5);
    require(current == &stream, "New connection cannot acquire its stream");
    XLinkStreamReset(current);
    releaseStream(current);
    target = nullptr;
    require(staleRejected, "Old lookup returned a new connection's stream");
}

}

int main() {
    try {
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "Cannot initialize XLink");
        for(bool server : {false, true}) {
            for(bool response : {false, true}) {
                for(bool byName : {false, true}) closeWithWaiter(server, response, byName);
            }
        }
        lookupAcrossLinkReuse(false);
        lookupAcrossLinkReuse(true);
        std::puts("Concurrent close lookup, link reset, stale identity, slot reuse and link reuse passed");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
