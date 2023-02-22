/**
 * @file    tcpip_host.c
 * @brief   TCP/IP helper definitions
*/

#include "tcpip_host.h"

#include <cstring>
#include <string>
#include <ctime>
#include <cstdio>
#include <functional>
#include <thread>
#include <mutex>
#include <cassert>
#include <chrono>
#include <atomic>
#include <cstddef>


#define MVLOG_UNIT_NAME tcpip_host
#include "XLinkLog.h"

// Windows specifics
#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <Iphlpapi.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#define errno WSAGetLastError()
#define ERRNO_EAGAIN WSAETIMEDOUT

#else

// *Unix specifics
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#define ERRNO_EAGAIN EAGAIN

#endif

/* Host-to-device command list */
typedef enum
{
    TCPIP_HOST_CMD_NO_COMMAND = 0,
    TCPIP_HOST_CMD_DEVICE_DISCOVER = 1,
    TCPIP_HOST_CMD_DEVICE_INFO = 2,
    TCPIP_HOST_CMD_RESET = 3,
    TCPIP_HOST_CMD_DEVICE_DISCOVERY_EX = 4,
} tcpipHostCommand_t;

/* Device state */
typedef enum
{
    TCPIP_HOST_STATE_INVALID = 0,
    TCPIP_HOST_STATE_BOOTED = 1,
    TCPIP_HOST_STATE_UNBOOTED = 2,
    TCPIP_HOST_STATE_BOOTLOADER = 3,
    TCPIP_HOST_STATE_FLASH_BOOTED = 4,
    TCPIP_HOST_STATE_BOOTED_NON_EXCLUSIVE = TCPIP_HOST_STATE_FLASH_BOOTED,
    TCPIP_HOST_STATE_GATE = 5,
    TCPIP_HOST_STATE_GATE_BOOTED = 6,
} tcpipHostDeviceState_t;

/* Device protocol */
typedef enum
{
    TCPIP_HOST_PROTOCOL_USB_VSC = 0,
    TCPIP_HOST_PROTOCOL_USB_CDC = 1,
    TCPIP_HOST_PROTOCOL_PCIE = 2,
    TCPIP_HOST_PROTOCOL_IPC = 3,
    TCPIP_HOST_PROTOCOL_TCP_IP = 4,
} tcpipHostDeviceProtocol_t;

/* Device platform */
typedef enum
{
    TCPIP_HOST_PLATFORM_INVALID = 0,
    TCPIP_HOST_PLATFORM_MYRIAD_X = 2,
    TCPIP_HOST_PLATFORM_RVC3 = 3,
} tcpipHostDevicePlatform_t;
/* Device response payload */
typedef struct
{
    tcpipHostCommand_t command;
    char mxid[32];
    uint32_t state;
} tcpipHostDeviceDiscoveryResp_t;

typedef struct
{
    tcpipHostCommand_t command;
    char mxid[32];
    int32_t linkSpeed;
    int32_t linkFullDuplex;
    int32_t gpioBootMode;
} tcpipHostDeviceInformationResp_t;

typedef struct
{
    tcpipHostCommand_t command;
    char id[32];
    uint32_t state;
    uint32_t protocol;
    uint32_t platform;
    uint16_t portHttp;
    uint16_t portHttps;
} tcpipHostDeviceDiscoveryExResp_t;

typedef struct
{
    tcpipHostCommand_t command;
} tcpipHostDeviceDiscoveryReq_t;


/* **************************************************************************/
/*      Private Macro Definitions                                            */
/* **************************************************************************/

// Debug, uncomment first line for some printing
//#define HAS_DEBUG
#undef HAS_DEBUG

static constexpr const auto DEFAULT_DEVICE_DISCOVERY_PORT = 11491;
static constexpr const auto DEFAULT_DEVICE_DISCOVERY_POOL_TIMEOUT = std::chrono::milliseconds{500};

constexpr int MSEC_TO_USEC(int x) { return x * 1000; }
static constexpr auto DEVICE_DISCOVERY_SOCKET_TIMEOUT = std::chrono::milliseconds{20};
static constexpr auto DEVICE_DISCOVERY_RES_TIMEOUT = std::chrono::milliseconds{200};

#ifdef HAS_DEBUG
#define DEBUG(...) do { printf(__VA_ARGS__); } while(0)
#else
#define DEBUG(fmt, ...) do {} while(0)
#endif


static tcpipHostDeviceState_t tcpip_convert_device_state(XLinkDeviceState_t state) {
    switch (state) {
        case XLinkDeviceState_t::X_LINK_BOOTED: return TCPIP_HOST_STATE_BOOTED;
        case XLinkDeviceState_t::X_LINK_UNBOOTED: return TCPIP_HOST_STATE_UNBOOTED;
        case XLinkDeviceState_t::X_LINK_BOOTLOADER: return TCPIP_HOST_STATE_BOOTLOADER;
        case XLinkDeviceState_t::X_LINK_BOOTED_NON_EXCLUSIVE: return TCPIP_HOST_STATE_BOOTED_NON_EXCLUSIVE;
        case XLinkDeviceState_t::X_LINK_GATE: return TCPIP_HOST_STATE_GATE;
        case XLinkDeviceState_t::X_LINK_GATE_BOOTED: return TCPIP_HOST_STATE_GATE_BOOTED;
        case XLinkDeviceState_t::X_LINK_ANY_STATE: return TCPIP_HOST_STATE_INVALID;
    }
    return TCPIP_HOST_STATE_INVALID;
}

static XLinkDeviceState_t tcpip_convert_device_state(uint32_t state)
{
    if(state == TCPIP_HOST_STATE_BOOTED)
    {
        return X_LINK_BOOTED;
    }
    else if(state == TCPIP_HOST_STATE_UNBOOTED)
    {
        return X_LINK_UNBOOTED;
    }
    else if(state == TCPIP_HOST_STATE_BOOTLOADER)
    {
        return X_LINK_BOOTLOADER;
    }
    else if(state == TCPIP_HOST_STATE_BOOTED_NON_EXCLUSIVE)
    {
        return X_LINK_BOOTED_NON_EXCLUSIVE;
    }
    else if(state == TCPIP_HOST_STATE_GATE)
    {
        return X_LINK_GATE;
    }
    else if(state == TCPIP_HOST_STATE_GATE_BOOTED)
    {
        return X_LINK_GATE_BOOTED;
    }
    else
    {
        return X_LINK_ANY_STATE;
    }
}

static XLinkProtocol_t tcpip_convert_device_protocol(uint32_t protocol)
{
    switch (protocol)
    {
    case TCPIP_HOST_PROTOCOL_USB_VSC: return X_LINK_USB_VSC;
    case TCPIP_HOST_PROTOCOL_USB_CDC: return X_LINK_USB_CDC;
    case TCPIP_HOST_PROTOCOL_PCIE: return X_LINK_PCIE;
    case TCPIP_HOST_PROTOCOL_IPC: return X_LINK_IPC;
    case TCPIP_HOST_PROTOCOL_TCP_IP: return X_LINK_TCP_IP;
    default:
        return X_LINK_ANY_PROTOCOL;
        break;
    }
}

static XLinkPlatform_t tcpip_convert_device_platform(uint32_t platform)
{
    switch (platform)
    {
    case TCPIP_HOST_PLATFORM_MYRIAD_X: return X_LINK_MYRIAD_X;
    case TCPIP_HOST_PLATFORM_RVC3: return X_LINK_RVC3;
    default:
        return X_LINK_ANY_PLATFORM;
        break;
    }
}

static tcpipHostDevicePlatform_t tcpip_convert_device_platform(XLinkPlatform_t platform) {
    switch (platform) {
        case X_LINK_MYRIAD_X: return TCPIP_HOST_PLATFORM_MYRIAD_X;
        case X_LINK_RVC3: return TCPIP_HOST_PLATFORM_RVC3;
        case X_LINK_ANY_PLATFORM: return TCPIP_HOST_PLATFORM_INVALID;
        case X_LINK_MYRIAD_2: return TCPIP_HOST_PLATFORM_INVALID;
    }
    return TCPIP_HOST_PLATFORM_INVALID;
}

static tcpipHostError_t tcpip_create_socket(TCPIP_SOCKET* out_sock, bool broadcast, int timeout_ms)
{
    TCPIP_SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 #if (defined(_WIN32) || defined(_WIN64) )
    if(sock == INVALID_SOCKET)
    {
        return TCPIP_HOST_ERROR;
    }
 #else
    if(sock < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // add socket option for broadcast
    int rc = 1;
    if(broadcast)
    {
        if(setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *) &rc, sizeof(rc)) < 0)
        {
            return TCPIP_HOST_ERROR;
        }
    }

#if (defined(_WIN32) || defined(_WIN64) )
#else
    int reuse_addr = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // Specify timeout
    if(timeout_ms != 0) {
#if (defined(_WIN32) || defined(_WIN64) )
        int read_timeout = timeout_ms;
#else
        struct timeval read_timeout;
        read_timeout.tv_sec = 0;
        read_timeout.tv_usec = MSEC_TO_USEC(timeout_ms);
#endif
        if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &read_timeout, sizeof(read_timeout)) < 0)
        {
            return TCPIP_HOST_ERROR;
        }
    }

    *out_sock = sock;
    return TCPIP_HOST_SUCCESS;
}

static tcpipHostError_t tcpip_create_socket_broadcast(TCPIP_SOCKET* out_sock, std::chrono::milliseconds timeout = std::chrono::milliseconds(0))
{
    return tcpip_create_socket(out_sock, true, static_cast<int>(timeout.count()));
}



static tcpipHostError_t tcpip_send_broadcast(TCPIP_SOCKET sock){

#if (defined(_WIN32) || defined(_WIN64) )

    DWORD rv, size = 0;
    PMIB_IPADDRTABLE ipaddrtable;

    rv = GetIpAddrTable(NULL, &size, 0);
    if (rv != ERROR_INSUFFICIENT_BUFFER) {
        return TCPIP_HOST_ERROR;
    }
    ipaddrtable = (PMIB_IPADDRTABLE) malloc(size);
    if (!ipaddrtable)
        return TCPIP_HOST_ERROR;

    rv = GetIpAddrTable(ipaddrtable, &size, 0);
    if (rv != NO_ERROR) {
        free(ipaddrtable);
        return TCPIP_HOST_ERROR;
    }

    tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
    struct sockaddr_in broadcast = { 0 };
    broadcast.sin_addr.s_addr = INADDR_BROADCAST;
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);
    for (DWORD i = 0; i < ipaddrtable->dwNumEntries; ++i) {

        // Get broadcast IP
        MIB_IPADDRROW addr = ipaddrtable->table[i];
        broadcast.sin_addr.s_addr = (addr.dwAddr & addr.dwMask)
            | (addr.dwMask ^ (DWORD)0xffffffff);
        sendto(sock, reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer), 0, (struct sockaddr*) & broadcast, sizeof(broadcast));

#ifdef HAS_DEBUG
        char ip_broadcast_str[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &((struct sockaddr_in*) & broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
        DEBUG("Interface up and running. Broadcast IP: %s\n", ip_broadcast_str);
#endif

    }

    free(ipaddrtable);

    return TCPIP_HOST_SUCCESS;

#else

    struct ifaddrs *ifaddr = NULL;
    if(getifaddrs(&ifaddr) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    // iterate linked list of interface information
    int socket_count = 0;
    for(struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        // check for ipv4 family, and assign only AFTER ifa_addr != NULL
        sa_family_t family;
        if(ifa->ifa_addr != NULL && ((family = ifa->ifa_addr->sa_family) == AF_INET))
        {
            // Check if interface is up and running
            struct ifreq if_req;
            strncpy(if_req.ifr_name, ifa->ifa_name, sizeof(if_req.ifr_name));
            ioctl(sock, SIOCGIFFLAGS, &if_req);

            DEBUG("interface name %s, (flags: %hu). ", ifa->ifa_name, if_req.ifr_flags);

            if((if_req.ifr_flags & IFF_UP) && (if_req.ifr_flags & IFF_RUNNING)){
                // Interface is up and running
                // Calculate broadcast address (IPv4, OR negated mask)
                struct sockaddr_in ip_broadcast = *((struct sockaddr_in*) ifa->ifa_addr);
                struct sockaddr_in ip_netmask = *((struct sockaddr_in*) ifa->ifa_netmask);
                ip_broadcast.sin_addr.s_addr |= ~ip_netmask.sin_addr.s_addr;

                #ifdef HAS_DEBUG
                    char ip_broadcast_str[INET_ADDRSTRLEN] = {0};
                    inet_ntop(family, &((struct sockaddr_in *)&ip_broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
                    DEBUG("Up and running. Broadcast IP: %s", ip_broadcast_str);
                #endif

                // send broadcast message
                struct sockaddr_in broadcast_addr;
                broadcast_addr.sin_family = family;
                broadcast_addr.sin_addr.s_addr = ip_broadcast.sin_addr.s_addr;
                broadcast_addr.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);

                tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
                if(sendto(sock, reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer), 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr)) < 0)
                {
                    // Ignore if not successful. The devices on that interface won't be found
                }
            } else {
                DEBUG("Not up and running.");
            }

            DEBUG("\n");

        }
    }
    // Release interface addresses
    freeifaddrs(ifaddr);

    return TCPIP_HOST_SUCCESS;
#endif
}


/* **************************************************************************/
/*      Public Function Definitions                                         */
/* **************************************************************************/

tcpipHostError_t tcpip_initialize() {
#if (defined(_WIN32) || defined(_WIN64))
    WSADATA wsa_data;
    int ret = WSAStartup(MAKEWORD(2,2), &wsa_data);
    if(ret != 0) {
        mvLog(MVLOG_FATAL, "Couldn't initialize Winsock DLL using WSAStartup function. (Return value: %d)", ret);
        return TCPIP_HOST_ERROR;
    }
#endif

    return TCPIP_HOST_SUCCESS;
}

tcpipHostError_t tcpip_close_socket(TCPIP_SOCKET sock)
{
#if (defined(_WIN32) || defined(_WIN64) )
    if(sock != INVALID_SOCKET)
    {
        closesocket(sock);
        return TCPIP_HOST_SUCCESS;
    }
#else
    if(sock != -1)
    {
        close(sock);
        return TCPIP_HOST_SUCCESS;
    }
#endif
    return TCPIP_HOST_ERROR;
}

xLinkPlatformErrorCode_t tcpip_get_devices(const deviceDesc_t in_deviceRequirements, deviceDesc_t* devices, size_t devices_size, unsigned int* device_count)
{
    // Name signifies ip in TCP_IP protocol case
    const char* target_ip = in_deviceRequirements.name;
    XLinkDeviceState_t target_state = in_deviceRequirements.state;
    XLinkPlatform_t target_platform = in_deviceRequirements.platform;
    const char* target_mxid = in_deviceRequirements.mxid;

    // Socket
    TCPIP_SOCKET sock;

    bool check_target_ip = false;
    bool has_target_ip = false;
    if(target_ip != NULL && strlen(target_ip) > 0){
        has_target_ip = true;
        if(!in_deviceRequirements.nameHintOnly){
            check_target_ip = true;
        }
    }

    bool check_target_mxid = false;
    if(target_mxid != NULL && strlen(target_mxid) > 0){
        check_target_mxid = true;
    }

    // Create socket first (also capable of doing broadcasts)
    if(tcpip_create_socket_broadcast(&sock, DEVICE_DISCOVERY_SOCKET_TIMEOUT) != TCPIP_HOST_SUCCESS){
        return X_LINK_PLATFORM_ERROR;
    }

    // If IP is specified, do UNICAST
    if(has_target_ip) {
        // TODO(themarpe) - Add IPv6 capabilities
        // send unicast device discovery
        struct sockaddr_in device_address;
        device_address.sin_family = AF_INET;
        device_address.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);

        // Convert address to binary
        #if (defined(_WIN32) || defined(__USE_W32_SOCKETS)) && (_WIN32_WINNT <= 0x0501)
            device_address.sin_addr.s_addr = inet_addr(target_ip);  // for XP
        #else
            inet_pton(AF_INET, target_ip, &device_address.sin_addr.s_addr);
        #endif

        tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;

        if(sendto(sock, reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer), 0, (struct sockaddr *) &device_address, sizeof(device_address)) < 0)
        {
            tcpip_close_socket(sock);
            return X_LINK_PLATFORM_ERROR;
        }
    }

    // If IP isn't enforced, do a broadcast
    if(!check_target_ip) {
        if (tcpip_send_broadcast(sock) != TCPIP_HOST_SUCCESS) {
            tcpip_close_socket(sock);
            return X_LINK_PLATFORM_ERROR;
        }
    }

    // loop to receive message response from devices
    int num_devices_match = 0;
    // Loop through all sockets and received messages that arrived
    auto t1 = std::chrono::steady_clock::now();
    do {
        if(num_devices_match >= (long) devices_size){
            // Enough devices matched, exit the loop
            break;
        }

        char ip_addr[INET_ADDRSTRLEN] = {0};
        // tcpipHostDeviceDiscoveryResp_t recv_buffer = {};
        uint8_t recv_buffer[1500] = {};
        struct sockaddr_in dev_addr;
        #if (defined(_WIN32) || defined(_WIN64) )
            int len = sizeof(dev_addr);
        #else
            socklen_t len = sizeof(dev_addr);
        #endif

        int ret = recvfrom(sock, (char *) &recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr*) & dev_addr, &len);
        if(ret > 0)
        {
            DEBUG("Received UDP response, length: %d\n", ret);
            tcpipHostCommand_t command = *reinterpret_cast<tcpipHostCommand_t*>(recv_buffer);
            XLinkDeviceState_t foundState = X_LINK_ANY_STATE;
            XLinkError_t status = X_LINK_SUCCESS;
            XLinkPlatform_t platform = X_LINK_MYRIAD_X;
            XLinkProtocol_t protocol = X_LINK_TCP_IP;
            char bufferId[64] = {};
            uint16_t port = TCPIP_LINK_SOCKET_PORT;
            DEBUG("Command %d\n", command);
            if(command == TCPIP_HOST_CMD_DEVICE_DISCOVER) {
                tcpipHostDeviceDiscoveryResp_t* discovery = reinterpret_cast<tcpipHostDeviceDiscoveryResp_t*>(recv_buffer);
                foundState = tcpip_convert_device_state(discovery->state);
                strncpy(bufferId, discovery->mxid, sizeof(bufferId));
            } else if(command == TCPIP_HOST_CMD_DEVICE_DISCOVERY_EX) {
                tcpipHostDeviceDiscoveryExResp_t* discoveryEx = reinterpret_cast<tcpipHostDeviceDiscoveryExResp_t*>(recv_buffer);
                foundState = tcpip_convert_device_state(discoveryEx->state);
                strncpy(bufferId, discoveryEx->id, sizeof(bufferId));
                protocol = tcpip_convert_device_protocol(discoveryEx->protocol);
                platform = tcpip_convert_device_platform(discoveryEx->platform);
                // TODO(status)
                platform = tcpip_convert_device_platform(discoveryEx->platform);
                port = discoveryEx->portHttp;
            } else {
                continue;
            }
            // Check that state matches
            DEBUG("target_state: %d, foundState: %d\n", target_state, foundState);
            if(target_state != X_LINK_ANY_STATE && foundState != target_state) continue;
            // Check that platform matches
            DEBUG("target_platform: %d, platform: %d\n", target_platform, platform});
            if(target_platform != X_LINK_ANY_PLATFORM && platform != target_platform) continue;

            // Correct device found, increase matched num and save details
            // Convert IP address in binary into string
            inet_ntop(AF_INET, &dev_addr.sin_addr, ip_addr, sizeof(ip_addr));

            // Check IP if needed
            if(check_target_ip && strcmp(target_ip, ip_addr) != 0){
                // IP doesn't match, skip this device
                continue;
            }
            // Check MXID if needed
            if(check_target_mxid && strcmp(target_mxid, bufferId)){
                // MXID doesn't match, skip this device
                continue;
            }

            // copy device information
            // Status
            devices[num_devices_match].status = status;
            // IP
            memset(devices[num_devices_match].name, 0, sizeof(devices[num_devices_match].name));
            strncpy(devices[num_devices_match].name, ip_addr, sizeof(devices[num_devices_match].name));
            // MXID
            memset(devices[num_devices_match].mxid, 0, sizeof(devices[num_devices_match].mxid));
            strncpy(devices[num_devices_match].mxid, bufferId, sizeof(devices[num_devices_match].mxid));
            // Platform
            devices[num_devices_match].platform = platform;
            // Protocol
            devices[num_devices_match].protocol = protocol;
            // State
            devices[num_devices_match].state = foundState;

            num_devices_match++;
        }
    } while(std::chrono::steady_clock::now() - t1 < DEVICE_DISCOVERY_RES_TIMEOUT);

    tcpip_close_socket(sock);

    // Filter out duplicates - routing table will decide through which interface the packets will traverse
    // TODO(themarpe) - properly separate interfaces.
    // Either bind to interface addr, or SO_BINDTODEVICE Linux, IP_BOUND_IF macOS, and prefix interface name
    int write_index = 0;
    for(int i = 0; i < num_devices_match; i++){
        bool duplicate = false;
        for(int j = i - 1; j >= 0; j--){
            // Check if duplicate
            if(strcmp(devices[i].name, devices[j].name) == 0 && strcmp(devices[i].mxid, devices[j].mxid) == 0){
                duplicate = true;
                break;
            }
        }
        if(!duplicate){
            devices[write_index] = devices[i];
            write_index++;
        }
    }

    // if at least one device matched, return OK otherwise return not found
    if(num_devices_match <= 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
    // return total device found
    *device_count = write_index;

    // Return success if search was successful (if atleast on device was found)
    return X_LINK_PLATFORM_SUCCESS;
}


xLinkPlatformErrorCode_t tcpip_boot_bootloader(const char* name){
    if(name == NULL || name[0] == 0){
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }

    // Create socket for UDP unicast
    TCPIP_SOCKET sock;
    if(tcpip_create_socket(&sock, false, 100) != TCPIP_HOST_SUCCESS){
        return X_LINK_PLATFORM_ERROR;
    }

    // TODO(themarpe) - Add IPv6 capabilities
    // send unicast reboot to bootloader
    struct sockaddr_in device_address;
    device_address.sin_family = AF_INET;
    device_address.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);

    // Convert address to binary
    #if (defined(_WIN32) || defined(__USE_W32_SOCKETS)) && (_WIN32_WINNT <= 0x0501)
        device_address.sin_addr.s_addr = inet_addr(name);  // for XP
    #else
        inet_pton(AF_INET, name, &device_address.sin_addr.s_addr);
    #endif

    tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_RESET;
    if (sendto(sock, reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer), 0, (struct sockaddr *)&device_address, sizeof(device_address)) < 0)
    {
        return X_LINK_PLATFORM_ERROR;
    }

    tcpip_close_socket(sock);

    return X_LINK_PLATFORM_SUCCESS;
}


// Discovery Service
static std::thread serviceThread;
static std::mutex serviceMutex;
static std::atomic<bool> serviceRunning{false};
static std::function<void()> serviceCallback = nullptr;

void tcpip_set_discovery_service_reset_callback(void (*cb)()) {
    std::unique_lock<std::mutex> lock(serviceMutex);
    serviceCallback = cb;
}

xLinkPlatformErrorCode_t tcpip_start_discovery_service(const char* id, XLinkDeviceState_t state, XLinkPlatform_t platform) {
    // Parse arguments first
    auto deviceState = tcpip_convert_device_state(state);
    auto devicePlatform = tcpip_convert_device_platform(platform);
    if(deviceState == TCPIP_HOST_STATE_INVALID || devicePlatform == TCPIP_HOST_PLATFORM_INVALID) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
    if(id == nullptr) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
    std::string deviceId{id};
    auto resetCb = serviceCallback;

    // Lock and proceed to start the thread
    std::unique_lock<std::mutex> lock(serviceMutex);

    // The service must be stopped first
    if(serviceRunning) {
        return X_LINK_PLATFORM_ERROR;
    }
    if(serviceThread.joinable()) {
        serviceThread.join();
    }

    // Start service
    serviceRunning = true;
    serviceThread = std::thread([deviceId, deviceState, devicePlatform, resetCb](){
        // Historical artifact
        int gpioBootMode = 0x3;

        TCPIP_SOCKET sockfd;
        if(tcpip_create_socket_broadcast(&sockfd, DEFAULT_DEVICE_DISCOVERY_POOL_TIMEOUT) < 0)
        {
            mvLog(MVLOG_FATAL, "Failure creating socket. Couldn't start device discovery service");
            return;
        }

        struct sockaddr_in recv_addr;
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_port = htons(DEFAULT_DEVICE_DISCOVERY_PORT);
        recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if(bind(sockfd, (struct sockaddr*) &recv_addr, sizeof(recv_addr)) < 0)
        {
            mvLog(MVLOG_FATAL, "Failure binding. Couldn't start device discovery service");
            tcpip_close_socket(sockfd);
            return;
        }

        while(serviceRunning) {

            // receive broadcast message from client
            tcpipHostDeviceDiscoveryReq_t request;
            struct sockaddr_in send_addr;
            socklen_t socklen = sizeof(send_addr);

            #if (defined(_WIN32) || defined(_WIN64) )
                using PACKET_LEN = int;
            #else
                using PACKET_LEN = ssize_t;
            #endif

            PACKET_LEN packetlen = 0;
            if(( packetlen = recvfrom(sockfd, reinterpret_cast<char*>(&request), sizeof(request), 0, (struct sockaddr*) &send_addr, &socklen)) < 0){
                if(errno != ERRNO_EAGAIN) {
                    mvLog(MVLOG_ERROR, "Device discovery service - Error recvform - %d\n", errno);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            // Debug
            #ifdef HAS_DEBUG
                DEBUG("Received packet, length: %d data: ", packetlen);
                for(ssize_t i = 0; i < packetlen; i++){
                    DEBUG("%02X ", ((uint8_t*)&request)[i]);
                }
                DEBUG("\n");
            #endif

            // Parse Request
            switch (request.command) {
                case TCPIP_HOST_CMD_DEVICE_DISCOVER: {
                    mvLog(MVLOG_DEBUG, "Received device discovery request, sending back - mxid: %s, state: %u\n", deviceId.c_str(), (uint32_t) deviceState);

                    // send back device discovery response
                    tcpipHostDeviceDiscoveryResp_t resp = {};
                    resp.command = TCPIP_HOST_CMD_DEVICE_DISCOVER;
                    strncpy(resp.mxid, deviceId.c_str(), sizeof(resp.mxid));
                    resp.state = deviceState;

                    if(sendto(sockfd, reinterpret_cast<char*>(&resp), sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        mvLog(MVLOG_ERROR, "Device discovery service - Error sendto...\n");
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }

                } break;

                case TCPIP_HOST_CMD_DEVICE_INFO: {

                    mvLog(MVLOG_DEBUG, "Received device information request, sending back - mxid: %s, speed %d?, full duplex: %d?, boot mode: 0x%02X\n", deviceId.c_str(), 0,0, gpioBootMode);

                    // send back device information response
                    tcpipHostDeviceInformationResp_t resp = {};
                    resp.command = TCPIP_HOST_CMD_DEVICE_INFO;
                    strncpy(resp.mxid, deviceId.c_str(), sizeof(resp.mxid));
                    // TODO(themarpe) - reimplement or drop this command
                    resp.linkSpeed = 0;
                    resp.linkFullDuplex = 0;
                    resp.gpioBootMode = gpioBootMode;
                    if(sendto(sockfd, reinterpret_cast<char*>(&resp), sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        mvLog(MVLOG_ERROR, "Device discovery service - Error sendto...\n");
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }

                } break;

                case TCPIP_HOST_CMD_RESET: {
                    if(resetCb) resetCb();
                } break;

                // TODO(themarpe) - handle
                // case TCPIP_HOST_CMD_DEVICE_DISCOVERY_EX: {
                // } break;

                default: {

                    mvLog(MVLOG_DEBUG, "Received invalid request, sending back no_command");

                    // send back device information response
                    tcpipHostCommand_t resp = TCPIP_HOST_CMD_NO_COMMAND;
                    if(sendto(sockfd, reinterpret_cast<char*>(&resp), sizeof(resp), 0, (struct sockaddr*) &send_addr, sizeof(send_addr)) < 0) {
                        mvLog(MVLOG_ERROR, "Device discovery service - Error sendto...\n");
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }

                } break;

            }
        }
    });

    return X_LINK_PLATFORM_SUCCESS;
}

void tcpip_stop_discovery_service() {
    std::unique_lock<std::mutex> lock(serviceMutex);
    serviceRunning = false;
    serviceThread.join();
}

void tcpip_detach_discovery_service() {
    std::unique_lock<std::mutex> lock(serviceMutex);
    serviceThread.detach();
}

bool tcpip_is_running_discovery_service() {
    return serviceRunning;
}
