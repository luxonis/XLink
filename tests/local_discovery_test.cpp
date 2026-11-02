#include "../src/pc/protocols/local_memshd.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
bool socketExists = true;
unsigned accessCalls = 0;
unsigned connectionCalls = 0;
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
deviceDesc_t localRequest() {
    deviceDesc_t request{};
    request.protocol = X_LINK_LOCAL_SHDMEM;
    request.platform = X_LINK_ANY_PLATFORM;
    request.state = X_LINK_ANY_STATE;
    return request;
}
void checkRejected(const deviceDesc_t& request) {
    deviceDesc_t output;
    std::memset(&output, 0x5a, sizeof(output));
    const auto sentinel = output;
    unsigned count = 99;
    const auto status = shdmemGetDevices(request, &output, 1, &count);
    if(status == X_LINK_PLATFORM_SUCCESS)
        std::cerr << "Unexpected local match platform=" << output.platform << " state=" << output.state << " protocol=" << output.protocol
                  << " id=" << std::string(output.mxid, strnlen(output.mxid, sizeof(output.mxid))) << '\n';
    require(status == X_LINK_PLATFORM_DEVICE_NOT_FOUND, "Shared memory discovery impersonated a requested remote device");
    require(count == 0, "Unmatched shared memory lookup returned a device count");
    require(std::memcmp(&output, &sentinel, sizeof(output)) == 0, "Unmatched shared memory lookup overwrote its output");
}
void checkLocal(const deviceDesc_t& request) {
    deviceDesc_t output{};
    unsigned count = 99;
    require(shdmemGetDevices(request, &output, 1, &count) == X_LINK_PLATFORM_SUCCESS && count == 1,
            "Explicit local lookup lost its supported empty-ID behavior");
    require(output.mxid[0] == '\0' && std::strcmp(output.name, "/tmp/xlink.sock") == 0,
            "Local lookup invented a device identity or changed its path");
    require(output.platform == X_LINK_MYRIAD_X && output.state == X_LINK_BOOTED && output.protocol == X_LINK_LOCAL_SHDMEM,
            "Local lookup changed its existing descriptor");
}
}

extern "C" int __wrap_access(const char* path, int mode) {
    require(std::strcmp(path, "/tmp/xlink.sock") == 0 && mode == F_OK, "Discovery attempted an unexpected filesystem query");
    ++accessCalls;
    if(socketExists) return 0;
    errno = ENOENT;
    return -1;
}
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) {
    ++connectionCalls;
    errno = ECONNREFUSED;
    return -1;
}

int main() {
    try {
        auto request = localRequest();
        request.protocol = X_LINK_ANY_PROTOCOL;
        request.platform = X_LINK_RVC2NG;
        std::strcpy(request.mxid, "REMOTE-CAMERA-ID");
        checkRejected(request);
        request = localRequest();
        std::strcpy(request.mxid, "ANOTHER-DEVICE-ID");
        checkRejected(request);
        for(auto platform : {X_LINK_RVC2NG, X_LINK_RVC3, X_LINK_RVC4, X_LINK_MYRIAD_2}) {
            request = localRequest();
            request.platform = platform;
            checkRejected(request);
        }
        for(auto state : {X_LINK_UNBOOTED, X_LINK_BOOTLOADER, X_LINK_GATE, X_LINK_GATE_BOOTED, X_LINK_GATE_SETUP}) {
            request = localRequest();
            request.state = state;
            checkRejected(request);
        }
        request = localRequest();
        std::strcpy(request.name, "192.0.2.1");
        checkRejected(request);
        request = localRequest();
        request.protocol = X_LINK_TCP_IP;
        checkRejected(request);
        checkLocal(localRequest());
        request = localRequest();
        request.platform = X_LINK_MYRIAD_X;
        request.state = X_LINK_BOOTED;
        std::strcpy(request.name, "/tmp/xlink.sock");
        checkLocal(request);
        request.nameHintOnly = true;
        std::strcpy(request.name, "previous-local-name");
        checkLocal(request);
        request = localRequest();
        request.protocol = X_LINK_TCP_IP_OR_LOCAL_SHDMEM;
        checkLocal(request);
        request.protocol = X_LINK_ANY_PROTOCOL;
        checkLocal(request);
        socketExists = false;
        deviceDesc_t output{};
        unsigned count = 99;
        require(shdmemGetDevices(localRequest(), &output, 1, &count) != X_LINK_PLATFORM_SUCCESS && count == 0,
                "Missing local endpoint returned a device");
        socketExists = true;
        for(int capacity : {0, -1}) {
            std::memset(&output, 0x5a, sizeof(output));
            const auto sentinel = output;
            count = 99;
            require(shdmemGetDevices(localRequest(), &output, capacity, &count) == X_LINK_PLATFORM_INVALID_PARAMETERS && count == 0,
                    "Invalid local output capacity was accepted");
            require(std::memcmp(&output, &sentinel, sizeof(output)) == 0, "Invalid capacity wrote outside the local output array");
        }
        count = 99;
        require(shdmemGetDevices(localRequest(), nullptr, 1, &count) == X_LINK_PLATFORM_INVALID_PARAMETERS && count == 0,
                "Null local output was accepted");
        require(shdmemGetDevices(localRequest(), &output, 1, nullptr) == X_LINK_PLATFORM_INVALID_PARAMETERS,
                "Null local device count was accepted");
        require(accessCalls > 0 && connectionCalls == 0, "Discovery consumed a local listener or skipped endpoint existence");
        std::cout << "Local discovery filters, bounds and empty-ID lookup passed without connecting\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
