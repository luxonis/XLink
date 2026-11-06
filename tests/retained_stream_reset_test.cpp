#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
#include "XLink/XLinkStream.h"
}

namespace {
constexpr streamId_t streamId = 5;
std::atomic<bool> writeBlocked{false};
std::atomic<bool> releaseReader{false};
std::atomic<bool> linkClosed{false};

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(1);
}

void waitFor(const std::atomic<bool>& flag, const char* message) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!flag && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(flag, message);
}

int observeLocalResponse(xLinkEvent_t* event, xLinkEvent_t* response, bool server) {
    const int result = dispatcherLocalEventGetResponse(event, response, server);
    if (event->header.type == XLINK_WRITE_REQ && event->header.flags.bitField.block) writeBlocked = true;
    return result;
}

int sendResetAndUnblockWrite(xLinkEvent_t* event, XLinkTimespec*) {
    require(event->header.type == XLINK_RESET_REQ, "An unsent retained payload reached the transport");
    require(DispatcherUnblockEvent(-1, XLINK_WRITE_REQ, streamId, event->deviceHandle.xLinkFD) == 1,
            "Reset did not make the blocked write ready");
    releaseReader = true;
    return 0;
}

int receiveUntilReset(xLinkEvent_t*) {
    waitFor(releaseReader, "Dispatcher did not send reset");
    return -1;
}
}

int main() {
    XLinkGlobalHandler_t global{};
    require(XLinkInitialize(&global) == X_LINK_SUCCESS, "Cannot initialize XLink");
    controlFunctionTbl.localGetResponse = observeLocalResponse;
    controlFunctionTbl.eventSend = sendResetAndUnblockWrite;
    controlFunctionTbl.eventReceive = receiveUntilReset;
    controlFunctionTbl.closeDeviceFd = [](xLinkDeviceHandle_t*) {};
    controlFunctionTbl.closeLink = [](void*, int) { linkClosed = true; };

    int identity = 0;
    auto& link = availableXLinks[0];
    link.id = 0;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &identity;
    link.deviceHandle.protocol = X_LINK_TCP_IP;
    auto& stream = link.availableStreams[0];
    require(XLinkStreamInitialize(&stream, streamId, "retained") == X_LINK_SUCCESS, "Cannot initialize retained stream");
    stream.writeSize = 1;
    stream.remoteFillLevel = 1;
    releaseStream(&stream);
    require(XLink_sem_init(&link.dispatcherClosedSem, 0, 0) == 0, "Cannot initialize link close semaphore");
    require(DispatcherStart(&link) == X_LINK_SUCCESS, "Cannot start dispatcher");

    const uint8_t payload = 0xd4;
    std::atomic<unsigned> releases{0};
    XLinkError_t result = X_LINK_SUCCESS;
    std::thread writer([&] {
        result = XLinkWriteDataRetained(streamId, &payload, sizeof(payload), nullptr, 0, &releases, [](void* owner) {
            ++*static_cast<std::atomic<unsigned>*>(owner);
        });
    });

    waitFor(writeBlocked, "Retained write did not block on the full stream");
    xLinkEvent_t reset{};
    reset.header.type = XLINK_RESET_REQ;
    reset.deviceHandle = link.deviceHandle;
    require(DispatcherAddEvent(EVENT_LOCAL, &reset), "Cannot queue dispatcher reset");

    writer.join();
    waitFor(linkClosed, "Dispatcher did not close after reset");
    require(result == X_LINK_COMMUNICATION_FAIL, "Reset acknowledged a retained write without sending it");
    require(releases == 1, "Reset did not release the unsent retained payload exactly once");
    return 0;
}
