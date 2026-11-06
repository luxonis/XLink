#include "XLink/XLink.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t payloadSize = 2 * 1024 * 1024 + 43;
constexpr uint8_t trailer[] = {0x32, 0x04, 0xfd};

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(1);
}

std::string endpoint() {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    require(socketFd >= 0, "Cannot create loopback socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Cannot reserve loopback port");
    socklen_t length = sizeof(address);
    require(getsockname(socketFd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Cannot read loopback port");
    close(socketFd);
    return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

uint8_t valueAt(size_t offset) {
    return static_cast<uint8_t>((offset * 23 + 19) % 251);
}

struct Owner {
    explicit Owner(std::atomic<unsigned>& releases) : releases(releases), bytes(payloadSize) {
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = valueAt(i);
    }

    static void release(void* opaque) {
        auto* owner = static_cast<Owner*>(opaque);
        auto& releases = owner->releases;
        delete owner;
        ++releases;
    }

    std::atomic<unsigned>& releases;
    std::vector<uint8_t> bytes;
};
}

int main() {
    std::atomic<unsigned> releases{0};
    require(XLinkWriteDataRetained(INVALID_STREAM_ID, nullptr, 0, nullptr, 0, new Owner(releases), Owner::release) != X_LINK_SUCCESS,
            "Malformed retained write succeeded");
    require(releases == 1, "Malformed retained write did not release owner");

    XLinkGlobalHandler_t global{};
    require(XLinkInitialize(&global) == X_LINK_SUCCESS, "Cannot initialize XLink");
    std::string address = endpoint();
    std::thread server([&] {
        XLinkHandler_t handler{};
        handler.devicePath = &address[0];
        handler.protocol = X_LINK_TCP_IP;
        require(XLinkServerOnly(&handler) == X_LINK_SUCCESS, "Cannot accept retained stream client");
        const auto stream = XLinkOpenStream(handler.linkId, "retained", payloadSize + sizeof(trailer));
        require(stream != INVALID_STREAM_ID, "Cannot open receiver stream");
        streamPacketDesc_t* packet = nullptr;
        require(XLinkReadData(stream, &packet) == X_LINK_SUCCESS && packet, "Cannot read retained stream payload");
        require(packet->length == payloadSize + sizeof(trailer), "Retained stream payload length changed");
        for (size_t i = 0; i < payloadSize; ++i) require(packet->data[i] == valueAt(i), "Retained stream payload bytes changed");
        require(std::memcmp(packet->data + payloadSize, trailer, sizeof(trailer)) == 0, "Retained stream trailer changed");
        require(XLinkReleaseData(stream) == X_LINK_SUCCESS, "Cannot release received packet");
    });

    XLinkHandler_t handler{};
    handler.devicePath = &address[0];
    handler.protocol = X_LINK_TCP_IP;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XLinkError_t connection = X_LINK_ERROR;
    while (connection != X_LINK_SUCCESS && std::chrono::steady_clock::now() < deadline) {
        connection = XLinkConnect(&handler);
        if (connection != X_LINK_SUCCESS) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(connection == X_LINK_SUCCESS, "Cannot connect to retained stream server");
    const auto stream = XLinkOpenStream(handler.linkId, "retained", payloadSize + sizeof(trailer));
    require(stream != INVALID_STREAM_ID, "Cannot open sender stream");
    auto* owner = new Owner(releases);
    require(XLinkWriteDataRetained(stream, owner->bytes.data(), owner->bytes.size(), trailer, sizeof(trailer), owner, Owner::release) ==
                X_LINK_SUCCESS,
            "Retained stream write failed");
    server.join();

    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (releases < 2 && std::chrono::steady_clock::now() < releaseDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(releases == 2, "Retained stream owner was released too early or more than once");
    require(XLinkResetRemote(handler.linkId) == X_LINK_SUCCESS, "Cannot disconnect retained stream");
    std::puts("Retained write round trip preserved payload and trailer and released ownership");
}
