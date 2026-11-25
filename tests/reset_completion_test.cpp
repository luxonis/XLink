#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
}

namespace {
void require(bool condition, const char* message) {
    if(condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(1);
}

struct Peer {
    std::mutex mutex;
    std::condition_variable changed;
    xLinkEvent_t response{};
    bool acknowledge = true;
    bool responseReady = false;
    bool closed = false;
    unsigned receives = 0;
    bool server = true;
    bool acknowledged = false;
    unsigned resets = 0;
    unsigned closes = 0;
} peer;

int sendReset(xLinkEvent_t* event, XLinkTimespec*) {
    std::unique_lock<std::mutex> lock(peer.mutex);
    require(event->header.type == XLINK_RESET_REQ, "Unexpected request reached the reset peer");
    ++peer.resets;
    if(peer.acknowledge) {
        peer.response = *event;
        peer.response.header.type = XLINK_RESET_RESP;
        peer.response.header.flags.raw = 0;
        peer.response.header.flags.bitField.ack = 1;
        peer.responseReady = true;
        peer.changed.notify_all();
        if(peer.server)
            require(peer.changed.wait_for(lock, std::chrono::seconds(2), [] { return peer.receives >= 2; }),
                    "Reader did not queue the acknowledgment while the sender was busy");
    }
    return 0;
}

int receiveResponse(xLinkEvent_t* event) {
    std::unique_lock<std::mutex> lock(peer.mutex);
    ++peer.receives;
    peer.changed.notify_all();
    peer.changed.wait(lock, [] { return peer.responseReady || peer.closed; });
    if(!peer.responseReady) return -1;
    *event = peer.response;
    peer.responseReady = false;
    return 0;
}

int handleResponse(xLinkEvent_t* event, xLinkEvent_t* response, bool server) {
    const int result = dispatcherRemoteEventGetResponse(event, response, server);
    if(event->header.type == XLINK_RESET_RESP) {
        std::lock_guard<std::mutex> lock(peer.mutex);
        peer.acknowledged = true;
    }
    return result;
}

void closePeer(xLinkDeviceHandle_t*) {
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.closed = true;
    ++peer.closes;
    peer.changed.notify_all();
}
}

int main(int argc, char** argv) {
    require(argc == 1 || argc == 4, "Expected server/client, timed/untimed and ack/silent arguments");
    const bool server = argc == 1 || std::strcmp(argv[1], "server") == 0;
    const bool timed = argc == 1 || std::strcmp(argv[2], "timed") == 0;
    peer.server = server;
    peer.acknowledge = argc == 1 || std::strcmp(argv[3], "ack") == 0;
    require(timed || peer.acknowledge, "An untimed silent-peer test cannot finish");

    XLinkGlobalHandler_t global{};
    global.loglevel = 2;
    require(XLinkInitialize(&global) == X_LINK_SUCCESS, "Cannot initialize XLink");
    controlFunctionTbl.eventSend = sendReset;
    controlFunctionTbl.eventReceive = receiveResponse;
    controlFunctionTbl.closeDeviceFd = closePeer;
    controlFunctionTbl.remoteGetResponse = handleResponse;
    controlFunctionTbl.closeLink = [](void*, int) {};

    int identity = 0;
    auto& link = availableXLinks[0];
    link.id = 0;
    link.peerState = XLINK_UP;
    link.deviceHandle.xLinkFD = &identity;
    link.deviceHandle.protocol = X_LINK_TCP_IP;
    require(XLink_sem_init(&link.dispatcherClosedSem, 0, 0) == 0, "Cannot initialize link completion");
    const auto started = server ? DispatcherStartServer(&link) : DispatcherStart(&link);
    require(started == X_LINK_SUCCESS, "Cannot start dispatcher");
    {
        std::unique_lock<std::mutex> lock(peer.mutex);
        require(peer.changed.wait_for(lock, std::chrono::seconds(2), [] { return peer.receives != 0; }),
                "Dispatcher reader did not start");
    }

    const auto actual = timed ? XLinkResetRemoteTimeout(0, 100) : XLinkResetRemote(0);
    const int expected = peer.acknowledge ? X_LINK_SUCCESS : server ? X_LINK_TIMEOUT : -1;
    if(actual != expected) std::fprintf(stderr, "Expected reset status %d, got %d\n", expected, actual);
    require(actual == expected, "Reset returned the wrong response status");
    std::lock_guard<std::mutex> lock(peer.mutex);
    require(!server || !peer.acknowledge || peer.acknowledged, "Server reset skipped its queued acknowledgment");
    require(peer.resets == 1 && peer.closes == 1 && peer.closed,
            "Reset did not send one request and close the connection exactly once");
    std::puts("Reset completed and closed the local connection");
    return 0;
}
