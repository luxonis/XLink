#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcher.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
#include "XLink/XLinkStream.h"
}

namespace {
struct ReceiveState {
    alignas(64) std::array<unsigned char, 128> bytes{};
    bool fail = false;
    unsigned allocated = 0;
    unsigned freed = 0;
    unsigned headers = 0;
    unsigned payloads = 0;
    std::atomic<bool> closed{false};
    std::atomic<bool> cleaned{false};
    unsigned fillAtClose = 0;
} state;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

void* allocate(uint32_t size, uint32_t alignment, void*) {
    ++state.allocated;
    if(state.fail) return nullptr;
    require(size == 64 && alignment == 64, "Receive allocation size or alignment changed");
    return state.bytes.data();
}

void deallocate(void* data, uint32_t size, uint32_t alignment, void*) {
    require(data == state.bytes.data() && size == 64 && alignment == 64, "Moved receive lost allocator identity");
    ++state.freed;
}

int send(xLinkEvent_t*, XLinkTimespec*) { return 0; }

void closeDevice(xLinkDeviceHandle_t*) {
    state.fillAtClose = availableXLinks[0].availableStreams[0].localFillLevel;
    state.closed.store(true);
}

void closeLink(void* fd, int fullClose) {
    dispatcherCloseLink(fd, fullClose);
    state.cleaned.store(true);
}
}

extern "C" int __wrap_XLinkPlatformRead(xLinkDeviceHandle_t*, void* data, int size, long* fd) {
    *fd = -1;
    if(state.closed.load()) return -1;
    if(size == sizeof(xLinkEventHeader_t)) {
        xLinkEventHeader_t header{};
        header.type = state.headers++ == 0 ? XLINK_WRITE_REQ : XLINK_PING_REQ;
        header.streamId = 11;
        header.size = 4;
        std::memcpy(data, &header, sizeof(header));
        if(state.headers > 2) {
            std::this_thread::yield();
            return -1;
        }
    } else {
        ++state.payloads;
        require(data == state.bytes.data() && size == 4, "Payload read bypassed custom allocation");
        const unsigned char payload[] = {0x12, 0x34, 0x56, 0x78};
        std::memcpy(data, payload, sizeof(payload));
    }
    return size;
}

int main(int argc, char** argv) {
    try {
        state.fail = argc > 1 && std::strcmp(argv[1], "failure") == 0;
        require(XLinkSetDataAllocator(allocate, deallocate, nullptr) == X_LINK_SUCCESS, "Cannot install receive allocator");
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "Cannot initialize XLink");
        auto& link = availableXLinks[0];
        int identity = 0;
        link.id = 0;
        link.peerState = XLINK_UP;
        link.deviceHandle.xLinkFD = &identity;
        auto& stream = link.availableStreams[0];
        require(XLinkStreamInitialize(&stream, 11, "receive") == X_LINK_SUCCESS, "Cannot initialize receive stream");
        stream.readSize = 1024;
        stream.writeSize = 64;
        releaseStream(&stream);
        require(XLink_sem_init(&link.dispatcherClosedSem, 0, 0) == 0, "Cannot initialize link completion");

        if(state.fail) {
            DispatcherControlFunctions controls{send, dispatcherEventReceive, dispatcherLocalEventGetResponse,
                                               dispatcherRemoteEventGetResponse, closeLink, closeDevice};
            require(DispatcherInitialize(&controls) == X_LINK_SUCCESS, "Cannot initialize dispatcher");
            const auto startStatus = argc > 2 ? DispatcherStartServer(&link) : DispatcherStart(&link);
            require(startStatus == X_LINK_SUCCESS, "Cannot start receive dispatcher");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while(!state.cleaned.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            if(!state.cleaned.load()) {
                std::fprintf(stderr, "Allocation failure did not terminate connection, transport closed = %d\n", state.closed.load());
                std::_Exit(1);
            }
            require(state.closed.load(), "Allocation failure did not close transport");
            require(state.fillAtClose == 0, "Allocation failure leaked stream fill level");
            require(state.headers == 1 && state.payloads == 0, "Unread payload was consumed as another header");
            require(state.allocated == 1 && state.freed == 0, "Failed allocation ownership mismatch");
            std::puts("Allocation failure closes transport and dispatcher without another header or leaked fill level");
        } else {
            xLinkEvent_t receive{};
            receive.deviceHandle = link.deviceHandle;
            require(dispatcherEventReceive(&receive) == 0, "Receive failed");
            require(stream.localFillLevel == 4 && stream.availablePackets == 1, "Receive packet accounting changed");
            xLinkEvent_t read{};
            xLinkEvent_t response{};
            read.deviceHandle = link.deviceHandle;
            read.header.type = XLINK_READ_REQ;
            read.header.streamId = 11;
            read.header.flags.bitField.moveSemantic = 1;
            require(dispatcherLocalEventGetResponse(&read, &response, false) == 0 && read.data != nullptr, "Cannot move received packet");
            auto packet = *static_cast<streamPacketDesc_t*>(read.data);
            require(packet.data == state.bytes.data() && packet.length == 4, "Received packet does not own custom storage");
            const unsigned char expected[] = {0x12, 0x34, 0x56, 0x78};
            require(std::memcmp(packet.data, expected, sizeof(expected)) == 0, "Received payload bytes changed");
            dispatcherCloseLink(&identity, 1);
            require(state.freed == 0, "Closing connection freed moved receive storage");
            XLinkDeallocateMoveData(packet.data, packet.length);
            require(state.allocated == 1 && state.freed == 1 && state.payloads == 1, "Moved receive did not free through callback once");
            std::puts("Received packet uses custom storage, survives connection close, and frees through callback once");
        }
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
