// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>
#include <stdbool.h>

#include "XLinkPlatform.h"
#include "XLinkPlatformErrorUtils.h"
#include "usb_boot.h"
#include "pcie_host.h"
#include "tcpip_host.h"
#include "XLinkStringUtils.h"
#include "PlatformDeviceFd.h"

#define MVLOG_UNIT_NAME PlatformDeviceControl
#include "XLinkLog.h"

#if (defined(_WIN32) || defined(_WIN64))
#include "win_usb.h"
#include "win_time.h"

#define OPEN_DEV_ERROR_MESSAGE_LENGTH 128

#else
#include <unistd.h>
#include <libusb.h>
#endif  /*defined(_WIN32) || defined(_WIN64)*/

#ifndef USE_USB_VSC
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>

int usbFdWrite = -1;
int usbFdRead = -1;
#endif  /*USE_USB_VSC*/

#include "XLinkPublicDefines.h"

#define USB_LINK_SOCKET_PORT 5678
#define UNUSED __attribute__((unused))


static UsbSpeed_t usb_speed_enum = X_LINK_USB_SPEED_UNKNOWN;
static char mx_serial[XLINK_MAX_MX_ID_SIZE] = { 0 };
#ifdef USE_USB_VSC
static const int statuswaittimeout = 5;
#endif

typedef struct {
  uint8_t  requestType;
  uint8_t  request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
} UsbSetupPacket;

static UsbSetupPacket bootBootloaderPacket = {
    .requestType = 0x00, // bmRequestType: device-directed
    .request = 0xF5, // bRequest: custom
    .value = 0x0DA1, // wValue: custom
    .index = 0x0000, // wIndex
    .length = 0 // not used
};

#ifdef USE_TCP_IP

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include <winsock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#endif /* USE_TCP_IP */

// ------------------------------------
// Helpers declaration. Begin.
// ------------------------------------

static char* pciePlatformStateToStr(const pciePlatformState_t platformState);

#ifdef USE_USB_VSC
static double seconds();
static libusb_device_handle *usbLinkOpen(const char *path);
static void usbLinkClose(libusb_device_handle *f);

#endif
// ------------------------------------
// Helpers declaration. End.
// ------------------------------------



// ------------------------------------
// Wrappers declaration. Begin.
// ------------------------------------

static int usbPlatformConnect(const char *devPathRead,
                              const char *devPathWrite, void **fd);
static int pciePlatformConnect(UNUSED const char *devPathRead,
                               const char *devPathWrite, void **fd);
static int tcpipPlatformConnect(const char *devPathRead,
                                const char *devPathWrite, void **fd);

static int usbPlatformBootBootloader(const char *name);
static int pciePlatformBootBootloader(const char *name);
static int tcpipPlatformBootBootloader(const char *name);

static int usbPlatformClose(void *fd);
static int pciePlatformClose(void *f);
static int tcpipPlatformClose(void *fd);

// ------------------------------------
// Wrappers declaration. End.
// ------------------------------------



// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

void XLinkPlatformInit()
{
#if (defined(_WIN32) || defined(_WIN64))
    initialize_usb_boot();

#ifdef USE_TCP_IP
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2,2), &wsa_data);
#endif

#endif
}


int XLinkPlatformBootRemote(const deviceDesc_t* deviceDesc, const char* binaryPath)
{
    FILE *file;
    long file_size;

    char *image_buffer;

    /* Open the mvcmd file */
    file = fopen(binaryPath, "rb");

    if(file == NULL) {
        mvLog(MVLOG_ERROR, "Cannot open file by path: %s", binaryPath);
        return -7;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    rewind(file);
    if(file_size <= 0 || !(image_buffer = (char*)malloc(file_size)))
    {
        mvLog(MVLOG_ERROR, "cannot allocate image_buffer. file_size = %ld", file_size);
        fclose(file);
        return -3;
    }
    if((long) fread(image_buffer, 1, file_size, file) != file_size)
    {
        mvLog(MVLOG_ERROR, "cannot read file to image_buffer");
        fclose(file);
        free(image_buffer);
        return -7;
    }
    fclose(file);

    if(XLinkPlatformBootFirmware(deviceDesc, image_buffer, file_size)) {
        free(image_buffer);
        return -1;
    }

    free(image_buffer);
    return 0;
}

int XLinkPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length) {
    if (deviceDesc->protocol == X_LINK_PCIE) {
        // Temporary open fd to boot device and then close it
        int* pcieFd = NULL;
        int rc = pcie_init(deviceDesc->name, (void**)&pcieFd);
        if (rc) {
            return rc;
        }
#if (!defined(_WIN32) && !defined(_WIN64))
        rc = pcie_boot_device(*(int*)pcieFd, firmware, length);
#else
        rc = pcie_boot_device(pcieFd, firmware, length);
#endif
        pcie_close(pcieFd); // Will not check result for now
        return rc;
    } else if (deviceDesc->protocol == X_LINK_USB_VSC) {

        char subaddr[28+2];
        // This will be the string to search for in /sys/dev/char links
        int chars_to_write = snprintf(subaddr, 28, "-%s:", deviceDesc->name);
        if(chars_to_write >= 28) {
            printf("Path to your boot util is too long for the char array here!\n");
        }
        // Boot it
        int rc = usb_boot(deviceDesc->name, firmware, (unsigned)length);

        if(!rc) {
            mvLog(MVLOG_DEBUG, "Boot successful, device address %s", deviceDesc->name);
        }
        return rc;
    } else {
        return -1;
    }
}


int XLinkPlatformConnect(const char* devPathRead, const char* devPathWrite, XLinkProtocol_t protocol, void** fd)
{
    switch (protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return usbPlatformConnect(devPathRead, devPathWrite, fd);

        case X_LINK_PCIE:
            return pciePlatformConnect(devPathRead, devPathWrite, fd);

        case X_LINK_TCP_IP:
            return tcpipPlatformConnect(devPathRead, devPathWrite, fd);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

int XLinkPlatformBootBootloader(const char* name, XLinkProtocol_t protocol)
{
    switch (protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return usbPlatformBootBootloader(name);

        case X_LINK_PCIE:
            return pciePlatformBootBootloader(name);

        case X_LINK_TCP_IP:
            return tcpipPlatformBootBootloader(name);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

int XLinkPlatformCloseRemote(xLinkDeviceHandle_t* deviceHandle)
{
    if(deviceHandle->protocol == X_LINK_ANY_PROTOCOL ||
       deviceHandle->protocol == X_LINK_NMB_OF_PROTOCOLS) {
        return X_LINK_PLATFORM_ERROR;
    }

    switch (deviceHandle->protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return usbPlatformClose(deviceHandle->xLinkFD);

        case X_LINK_PCIE:
            return pciePlatformClose(deviceHandle->xLinkFD);

        case X_LINK_TCP_IP:
            return tcpipPlatformClose(deviceHandle->xLinkFD);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

}

// ------------------------------------
// XLinkPlatform API implementation. End.
// ------------------------------------



// ------------------------------------
// Helpers implementation. Begin.
// ------------------------------------
#ifdef USE_USB_VSC
double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}
#endif

char* pciePlatformStateToStr(const pciePlatformState_t platformState) {
    switch (platformState) {
        case PCIE_PLATFORM_ANY_STATE: return "PCIE_PLATFORM_ANY_STATE";
        case PCIE_PLATFORM_BOOTED: return "PCIE_PLATFORM_BOOTED";
        case PCIE_PLATFORM_UNBOOTED: return "PCIE_PLATFORM_UNBOOTED";
        default: return "";
    }
}

#ifdef USE_USB_VSC


libusb_device* usbLinkFindDevice(const char* path){
    int rc = 0;
    if (path == NULL) {
        return 0;
    }

    libusb_device *dev = NULL;
    double waittm = seconds() + statuswaittimeout;

    int vid = DEFAULT_OPENVID;
    int pid = get_pid_by_name(path);

    while(seconds() < waittm){
        int size = (int)strlen(path);

#if (!defined(_WIN32) && !defined(_WIN64))
        uint16_t  bcdusb = -1;
        rc = usb_find_device_with_bcd(0, (char *)path, size, (void **)&dev, vid, pid, &bcdusb);
#else
        rc = usb_find_device(0, (char *)path, size, (void **)&dev, vid, pid);
#endif
        if(rc == USB_BOOT_SUCCESS)
            break;
        usleep(1000);
    }
    if (rc == USB_BOOT_TIMEOUT || rc == USB_BOOT_DEVICE_NOT_FOUND) // Timeout
        return 0;

    return dev;

}



libusb_device_handle *usbLinkOpen(const char *path)
{

    libusb_device *dev = usbLinkFindDevice(path);
    if(dev == NULL){
        return 0;
    }

    libusb_device_handle *h = NULL;

    // Retrieve mx id from name
    for(int i = 0; i < XLINK_MAX_NAME_SIZE; i++){
        if(path[i] == '-') break;
        mx_serial[i] = path[i];
    }

#if (defined(_WIN32) || defined(_WIN64) )

    char last_open_dev_err[OPEN_DEV_ERROR_MESSAGE_LENGTH] = {0};
    h = usb_open_device(dev, NULL, 0, last_open_dev_err, OPEN_DEV_ERROR_MESSAGE_LENGTH);
    int libusb_rc = ((h != NULL) ? (0) : (-1));
    if (libusb_rc < 0)
    {
        if(last_open_dev_err[0])
            mvLog(MVLOG_DEBUG, "Last opened device name: %s", last_open_dev_err);

        usb_close_device(h);
        usb_free_device(dev);
        return 0;
    }
    usb_free_device(dev);

    // Get usb speed
    usb_speed_enum = usb_get_usb_speed(h);

#else

    usb_speed_enum = libusb_get_device_speed(dev);

    int libusb_rc = libusb_open(dev, &h);
    if (libusb_rc < 0)
    {
        libusb_unref_device(dev);
        return 0;
    }

    libusb_unref_device(dev);
    libusb_detach_kernel_driver(h, 0);
    libusb_rc = libusb_claim_interface(h, 0);
    if(libusb_rc < 0)
    {
        libusb_close(h);
        return 0;
    }
#endif
    return h;
}

bool usbLinkBootBootloader(const char *path) {

    libusb_device *dev = usbLinkFindDevice(path);
    if(dev == NULL){
        return 0;
    }
    libusb_device_handle *h = NULL;


#if (defined(_WIN32) || defined(_WIN64) )

    char last_open_dev_err[OPEN_DEV_ERROR_MESSAGE_LENGTH] = {0};
    h = usb_open_device(dev, NULL, 0, last_open_dev_err, OPEN_DEV_ERROR_MESSAGE_LENGTH);
    int libusb_rc = ((h != NULL) ? (0) : (-1));
    if (libusb_rc < 0)
    {
        if(last_open_dev_err[0])
            mvLog(MVLOG_DEBUG, "Last opened device name: %s", last_open_dev_err);

        usb_close_device(h);
        usb_free_device(dev);
        return 0;
    }

    // Make control transfer
    uint32_t transferred = 0;
    usb_control_transfer(h,
        bootBootloaderPacket.requestType,   // bmRequestType: device-directed
        bootBootloaderPacket.request,   // bRequest: custom
        bootBootloaderPacket.value, // wValue: custom
        bootBootloaderPacket.index, // wIndex
        NULL,   // data pointer
        0,      // data size
        &transferred,
        1000    // timeout [ms]
    );

    // Ignore error, close the device
    usb_close_device(h);
    usb_free_device(dev);

#else

    int libusb_rc = libusb_open(dev, &h);
    if (libusb_rc < 0)
    {
        libusb_unref_device(dev);
        return 0;
    }

    // Make control transfer
    libusb_control_transfer(h,
        bootBootloaderPacket.requestType,   // bmRequestType: device-directed
        bootBootloaderPacket.request,   // bRequest: custom
        bootBootloaderPacket.value, // wValue: custom
        bootBootloaderPacket.index, // wIndex
        NULL,   // data pointer
        0,      // data size
        1000    // timeout [ms]
    );

    // Ignore error and close device
    libusb_unref_device(dev);
    libusb_close(h);

#endif

    return true;

}

void usbLinkClose(libusb_device_handle *f)
{
#if (defined(_WIN32) || defined(_WIN64))
    usb_close_device(f);
#else
    libusb_release_interface(f, 0);
    libusb_close(f);
#endif
}
#endif

/**
 * getter to obtain the connected usb speed which was stored by
 * usb_find_device_with_bcd() during XLinkconnect().
 * @note:
 *  getter will return empty or different value
 *  if called before XLinkConnect.
 */
UsbSpeed_t get_usb_speed(){
    return usb_speed_enum;
}

/**
 * getter to obtain the Mx serial id which was received by
 * usb_find_device_with_bcd() during XLinkconnect().
 * @note:
 *  getter will return empty or different value
 *  if called before XLinkConnect.
 */
const char* get_mx_serial(){
    #ifdef USE_USB_VSC
        return mx_serial;
    #else
        return "UNKNOWN";
    #endif
}

// ------------------------------------
// Helpers implementation. End.
// ------------------------------------



// ------------------------------------
// Wrappers implementation. Begin.
// ------------------------------------

int usbPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if (!defined(USE_USB_VSC))
    #ifdef USE_LINK_JTAG
    struct sockaddr_in serv_addr;
    usbFdWrite = socket(AF_INET, SOCK_STREAM, 0);
    usbFdRead = socket(AF_INET, SOCK_STREAM, 0);
    assert(usbFdWrite >=0);
    assert(usbFdRead >=0);
    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(USB_LINK_SOCKET_PORT);

    if (connect(usbFdWrite, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        mvLog(MVLOG_ERROR, "connect(usbFdWrite,...) returned < 0\n");
        if (usbFdRead >= 0)
            close(usbFdRead);
        if (usbFdWrite >= 0)
            close(usbFdWrite);
        usbFdRead = -1;
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    return 0;

#else
    usbFdRead= open(devPathRead, O_RDWR);
    if(usbFdRead < 0)
    {
        return X_LINK_PLATFORM_DEVICE_NOT_FOUND;
    }
    // set tty to raw mode
    struct termios  tty;
    speed_t     spd;
    int rc;
    rc = tcgetattr(usbFdRead, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdRead, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        usbFdRead = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    usbFdWrite= open(devPathWrite, O_RDWR);
    if(usbFdWrite < 0)
    {
        close(usbFdRead);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    // set tty to raw mode
    rc = tcgetattr(usbFdWrite, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }

    spd = B115200;
    cfsetospeed(&tty, (speed_t)spd);
    cfsetispeed(&tty, (speed_t)spd);

    cfmakeraw(&tty);

    rc = tcsetattr(usbFdWrite, TCSANOW, &tty);
    if (rc < 0) {
        close(usbFdRead);
        close(usbFdWrite);
        usbFdWrite = -1;
        return X_LINK_PLATFORM_ERROR;
    }
    return 0;
#endif  /*USE_LINK_JTAG*/
#else
    void* usbHandle = usbLinkOpen(devPathWrite);

    if (usbHandle == 0)
    {
        /* could fail due to port name change */
        return -1;
    }

    // Store the usb handle and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey(usbHandle);

#endif  /*USE_USB_VSC*/
}

int pciePlatformConnect(UNUSED const char *devPathRead,
                        const char *devPathWrite,
                        void **fd)
{
    return pcie_init(devPathWrite, fd);
}

// TODO add IPv6 to tcpipPlatformConnect()
int tcpipPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(USE_TCP_IP)
    if (!devPathWrite || !fd)
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    TCPIP_SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    // Disable sigpipe reception on send
    #if defined(SO_NOSIGPIPE)
        const int set = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
    #endif

    struct sockaddr_in serv_addr = { 0 };

    size_t len = strlen(devPathWrite);
    if (!len)
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    char *const devPathWriteBuff = (char *)malloc(len + 1);
    if (!devPathWriteBuff)
        return X_LINK_PLATFORM_ERROR;
    strncpy(devPathWriteBuff, devPathWrite, len);
    devPathWriteBuff[len] = 0;

    char* serv_ip = strtok(devPathWriteBuff, ":");
    char* serv_port = strtok(NULL, ":");

    // Parse port, or use default
    uint16_t port = TCPIP_LINK_SOCKET_PORT;
    if(serv_port != NULL){
        port = atoi(serv_port);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    int ret = inet_pton(AF_INET, serv_ip, &serv_addr.sin_addr);
    free(devPathWriteBuff);

    if(ret <= 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    if(connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) sock);

#endif
    return 0;
}


int usbPlatformBootBootloader(const char *name)
{
    if(usbLinkBootBootloader(name)){
        return 0;
    } else {
        return -1;
    }
}

int pciePlatformBootBootloader(const char *name)
{
    // TODO(themarpe)
    return -1;
}

int tcpipPlatformBootBootloader(const char *name)
{
    return tcpip_boot_bootloader(name);
}

int usbPlatformClose(void *fdKey)
{

#ifndef USE_USB_VSC
    #ifdef USE_LINK_JTAG
    /*Nothing*/
#else
    if (usbFdRead != -1){
        close(usbFdRead);
        usbFdRead = -1;
    }
    if (usbFdWrite != -1){
        close(usbFdWrite);
        usbFdWrite = -1;
    }
#endif  /*USE_LINK_JTAG*/
#else

    void* tmpUsbHandle = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpUsbHandle)){
        mvLog(MVLOG_FATAL, "Cannot find USB Handle by key");
        return -1;
    }
    usbLinkClose((libusb_device_handle *) tmpUsbHandle);

    if(destroyPlatformDeviceFdKey(fdKey)){
        mvLog(MVLOG_FATAL, "Cannot destroy USB Handle key");
        return -1;
    }

#endif  /*USE_USB_VSC*/
    return -1;
}

int pciePlatformClose(void *f)
{
    int rc;

    /**  For PCIe device reset is called on host side  */
#if (defined(_WIN32) || defined(_WIN64))
    rc = pcie_reset_device((HANDLE)f);
#else
    rc = pcie_reset_device(*(int*)f);
#endif
    if (rc) {
        mvLog(MVLOG_ERROR, "Device resetting failed with error %d", rc);
        pciePlatformState_t state = PCIE_PLATFORM_ANY_STATE;
        pcie_get_device_state(f, &state);
        mvLog(MVLOG_INFO, "Device state is %s", pciePlatformStateToStr(state));
    }
    rc = pcie_close(f);
    if (rc) {
        mvLog(MVLOG_ERROR, "Device closing failed with error %d", rc);
    }
    return rc;
}

int tcpipPlatformClose(void *fdKey)
{
#if defined(USE_TCP_IP)

    int status = 0;

    void* tmpsockfd = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpsockfd)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key");
        return -1;
    }
    TCPIP_SOCKET sock = (TCPIP_SOCKET) (uintptr_t) tmpsockfd;

#ifdef _WIN32
    status = shutdown(sock, SD_BOTH);
    if (status == 0) { status = closesocket(sock); }
#else
    if(sock != -1)
    {
        status = shutdown(sock, SHUT_RDWR);
        if (status == 0) { status = close(sock); }
    }
#endif

    if(destroyPlatformDeviceFdKey(fdKey)){
        mvLog(MVLOG_FATAL, "Cannot destroy file descriptor key");
        return -1;
    }

    return status;

#endif
    return -1;
}

// ------------------------------------
// Wrappers implementation. End.
// ------------------------------------
