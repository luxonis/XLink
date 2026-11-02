#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "XLink/XLink.h"
#include "XLink/XLinkDispatcherImpl.h"
#include "XLink/XLinkPrivateFields.h"
#include "XLink/XLinkStream.h"
}

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
struct Stream {
    int identity = 0;
    xLinkDesc_t& link = availableXLinks[0];
    streamDesc_t& stream = link.availableStreams[0];
    explicit Stream(streamId_t id) {
        link.id = 0;
        link.deviceHandle.xLinkFD = &identity;
        require(XLinkStreamInitialize(&stream, id, "asset") == X_LINK_SUCCESS, "Cannot initialize stream");
        stream.writeSize = 64;
        stream.readSize = 1024;
        releaseStream(&stream);
    }
    ~Stream() {
        if(XLink_sem_wait(&stream.sem) == 0) {
            XLinkStreamReset(&stream);
            releaseStream(&stream);
        }
    }
    xLinkEvent_t event(xLinkEventType_t type) {
        xLinkEvent_t result{};
        result.header.type = type;
        result.header.streamId = stream.id;
        result.deviceHandle = link.deviceHandle;
        return result;
    }
    void checkAccessible() {
        auto* retained = getStreamById(&identity, stream.id);
        require(retained == &stream, "A half-closed stream lost its synchronization state");
        releaseStream(retained);
    }
};
void closeDirections(bool server, bool peerFirst, streamId_t id) {
    Stream fixture(id);
    auto peer = fixture.event(XLINK_CLOSE_STREAM_REQ);
    auto local = fixture.event(XLINK_CLOSE_STREAM_RESP);
    local.header.flags.bitField.ack = 1;
    xLinkEvent_t response{};
    auto& first = peerFirst ? peer : local;
    auto& second = peerFirst ? local : peer;
    require(dispatcherRemoteEventGetResponse(&first, &response, server) == 0, "First close failed");
    require(fixture.stream.id == id, "Half-close retired the opposite direction");
    require(fixture.stream.writeSize == (peerFirst ? 64u : 0u), "Wrong writable direction after half-close");
    require(fixture.stream.readSize == (peerFirst ? 0u : 1024u), "Wrong readable direction after half-close");
    fixture.checkAccessible();
    require(dispatcherRemoteEventGetResponse(&second, &response, server) == 0, "Second close failed");
    require(fixture.stream.id == INVALID_STREAM_ID, "Closing both directions did not retire the stream");
}
void rejectedClose(bool server) {
    Stream fixture(51);
    fixture.stream.localFillLevel = 16;
    auto peer = fixture.event(XLINK_CLOSE_STREAM_REQ);
    xLinkEvent_t response{};
    require(dispatcherRemoteEventGetResponse(&peer, &response, server) == 0 && response.header.flags.bitField.nack,
            "Unread data did not reject peer close");
    auto local = fixture.event(XLINK_CLOSE_STREAM_RESP);
    local.header.flags.bitField.nack = 1;
    require(dispatcherRemoteEventGetResponse(&local, &response, server) == 0, "Rejected close response failed");
    require(fixture.stream.writeSize == 64 && fixture.stream.readSize == 1024, "Rejected close changed stream directions");
    fixture.checkAccessible();
}
void allocatedSlotReuse() {
    auto& link = availableXLinks[0];
    int identity = 0;
    link.id = 0;
    link.deviceHandle.xLinkFD = &identity;
    link.nextUniqueStreamId = 0;
    auto first = XLinkAddOrUpdateStream(&identity, "first", 64, 1024, INVALID_STREAM_ID);
    require(first == 0, "Unexpected initial stream id");
    auto* old = getStreamById(&identity, first);
    require(old != nullptr, "Allocated stream cannot be found");
    XLinkStreamReset(old);
    releaseStream(old);
    auto second = XLinkAddOrUpdateStream(&identity, "second", 64, 1024, INVALID_STREAM_ID);
    require(second != first && second != INVALID_STREAM_ID, "A new stream reused a stale id");
    require(getStreamById(&identity, first) == nullptr, "Retired id still resolves");
    auto* reused = getStreamById(&identity, second);
    require(reused == old, "Stream slot was not reused");
    XLinkStreamReset(reused);
    releaseStream(reused);

    link.nextUniqueStreamId = STREAM_ID_MASK;
    auto last = XLinkAddOrUpdateStream(&identity, "last", 64, 1024, INVALID_STREAM_ID);
    auto wrapped = XLinkAddOrUpdateStream(&identity, "wrapped", 64, 1024, INVALID_STREAM_ID);
    require(last == STREAM_ID_MASK && wrapped == 0, "Stream id crossed its wire bit field");
    for(auto id : {last, wrapped}) {
        auto* stream = getStreamById(&identity, id);
        require(stream != nullptr, "Wrapped stream cannot be found");
        XLinkStreamReset(stream);
        releaseStream(stream);
    }
}

}
int main() {
    try {
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "Cannot initialize XLink");
        for(bool server : {false, true}) {
            for(unsigned repeat = 0; repeat < 10; ++repeat) {
                closeDirections(server, true, 5 + repeat);
                closeDirections(server, false, 25 + repeat);
            }
            rejectedClose(server);
        }
        allocatedSlotReuse();
        std::puts("Stream half-close ordering, rejected close and slot reuse passed");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
