#include <XLink/XLink.h>
#include <XLink/XLinkPublicDefines.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Reply {
    uint32_t command = 4;
    std::array<char, 32> id{};
    uint32_t state = 5;
    uint32_t protocol = 4;
    uint32_t platform = 5;
    uint32_t status = 0;
    uint16_t httpPort = 11492;
    uint16_t httpsPort = 0;
};
static_assert(sizeof(Reply) == 56, "Gate discovery packet layout changed");
static_assert(X_LINK_MYRIAD_X == 2480 && X_LINK_RVC3 == 3000 && X_LINK_RVC4 == 4000, "Existing platform values must remain stable");

class GateReply {
    int socket = -1;
    std::thread worker;

   public:
    explicit GateReply(uint32_t platform) {
        socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        require(socket >= 0, "Cannot open loopback discovery socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(11491);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            close(socket);
            throw std::runtime_error("Loopback discovery port 11491 is unavailable");
        }
        timeval timeout{2, 0};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        worker = std::thread([this, platform]() {
            uint32_t command = 0;
            sockaddr_in sender{};
            socklen_t size = sizeof(sender);
            if (recvfrom(socket, &command, sizeof(command), 0, reinterpret_cast<sockaddr*>(&sender), &size) != sizeof(command) || command != 1) return;
            Reply reply;
            reply.platform = platform;
            std::strcpy(reply.id.data(), "RVC2NG-DISCOVERY-TEST");
            sendto(socket, &reply, sizeof(reply), 0, reinterpret_cast<const sockaddr*>(&sender), size);
        });
    }

    ~GateReply() {
        worker.join();
        close(socket);
    }
    GateReply(const GateReply&) = delete;
    GateReply& operator=(const GateReply&) = delete;
};

void checkDiscovery(uint32_t wirePlatform, XLinkPlatform_t filter, XLinkPlatform_t expected, bool matches = true) {
    GateReply gate(wirePlatform);
    deviceDesc_t requirements{};
    std::strcpy(requirements.name, "127.0.0.1");
    requirements.protocol = X_LINK_TCP_IP;
    requirements.platform = filter;
    requirements.state = X_LINK_ANY_STATE;
    deviceDesc_t found{};
    unsigned int count = 0;
    auto result = XLinkFindAllSuitableDevices(requirements, &found, 1, &count, 500);
    if (!matches) {
        require(result == X_LINK_DEVICE_NOT_FOUND && count == 0, "Platform filter accepted a different device");
        return;
    }
    require(result == X_LINK_SUCCESS && count == 1, "Gate discovery did not return exactly one device");
    require(found.platform == expected, "Incorrect platform decoded from the Gate response");
    require(found.state == X_LINK_GATE && found.protocol == X_LINK_TCP_IP, "Gate transport or state changed");
    require(std::strcmp(found.mxid, "RVC2NG-DISCOVERY-TEST") == 0, "Gate identity was lost");
}

}

int main() {
    try {
        XLinkGlobalHandler_t handler{};
        require(XLinkInitialize(&handler) == X_LINK_SUCCESS, "XLink initialization failed");
        checkDiscovery(2, X_LINK_ANY_PLATFORM, X_LINK_MYRIAD_X);
        checkDiscovery(3, X_LINK_ANY_PLATFORM, X_LINK_RVC3);
        checkDiscovery(4, X_LINK_ANY_PLATFORM, X_LINK_RVC4);
        checkDiscovery(5, X_LINK_ANY_PLATFORM, X_LINK_RVC2NG);
        checkDiscovery(5, X_LINK_RVC2NG, X_LINK_RVC2NG);
        checkDiscovery(5, X_LINK_RVC4, X_LINK_RVC2NG, false);
        checkDiscovery(99, X_LINK_RVC2NG, X_LINK_ANY_PLATFORM, false);
        require(std::strcmp(XLinkPlatformToStr(X_LINK_RVC2NG), "X_LINK_RVC2NG") == 0, "RVC2NG platform string is missing");
        std::cout << "Gate platform discovery and filtering passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
