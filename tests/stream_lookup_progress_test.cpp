#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <thread>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
#include "XLink/XLinkStream.h"
}

namespace {
std::atomic<bool> payloadStarted{false};
std::atomic<bool> finishPayload{false};
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
}

extern "C" int __wrap_XLinkPlatformRead(xLinkDeviceHandle_t*, void* data, int size, long* fd) {
    *fd = -1;
    if(size == sizeof(xLinkEventHeader_t)) {
        xLinkEventHeader_t header{};
        header.type = XLINK_WRITE_REQ;
        header.streamId = 11;
        header.size = 4;
        std::memcpy(data, &header, sizeof(header));
    } else {
        payloadStarted.store(true);
        while(!finishPayload.load()) std::this_thread::yield();
        std::memset(data, 0xAB, size);
    }
    return size;
}

int main() {
    try {
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "Cannot initialize XLink");
        auto& link = availableXLinks[0];
        int identity = 0;
        link.id = 0;
        link.peerState = XLINK_UP;
        link.deviceHandle.xLinkFD = &identity;
        for(unsigned index = 0; index < 2; ++index) {
            auto& stream = link.availableStreams[index];
            require(XLinkStreamInitialize(&stream, 11 + index, index == 0 ? "receiving" : "ready") == X_LINK_SUCCESS,
                    "Cannot initialize stream");
            stream.readSize = 1024;
            stream.writeSize = 64;
            releaseStream(&stream);
        }
        xLinkEvent_t receive{};
        receive.deviceHandle = link.deviceHandle;
        int receiveStatus = -1;
        std::thread reader([&] { receiveStatus = dispatcherEventReceive(&receive); });
        while(!payloadStarted.load()) std::this_thread::yield();
        std::promise<bool> lookupResult;
        auto result = lookupResult.get_future();
        std::thread lookup([&] {
            auto* byId = getStreamById(&identity, 12);
            bool valid = byId == &link.availableStreams[1];
            releaseStream(byId);
            auto* byName = getStreamByName(&link, "ready");
            valid = valid && byName == &link.availableStreams[1];
            releaseStream(byName);
            auto id = XLinkAddOrUpdateStream(&identity, "created", 64, 1024, INVALID_STREAM_ID);
            lookupResult.set_value(valid && id != INVALID_STREAM_ID);
        });
        bool progressed = result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        finishPayload.store(true);
        reader.join();
        lookup.join();
        require(receiveStatus == 0, "Receive failed");
        require(result.get(), "Unrelated stream lookup or creation failed");
        require(XLink_sem_init(&link.dispatcherClosedSem, 0, 0) == 0, "Cannot initialize link completion");
        dispatcherCloseLink(&identity, 1);
        require(progressed, "Unrelated stream lookup blocked behind payload receive");
        require(getLink(nullptr) == nullptr, "Null fd resolved to an unused link");
        require(getLink(&identity) == nullptr, "Closed fd still resolves to a link");
        std::puts("Unrelated stream lookup and creation progress during blocked receive; null and closed links rejected");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
