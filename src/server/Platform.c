#include "XLinkPlatform.h"

#include "stdio.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include <pthread.h>
#include <semaphore.h>

#include <stdbool.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif
#include "XLinkLog.h"
#include "XLinkMacros.h"

#ifdef XLINK_USE_MEMORY_MANAGER
#include "memManagerApi.h"
#endif


void startDeviceDiscoveryService(uint32_t deviceState);



#ifndef UNUSED
    #define UNUSED(X) ((void)X)
#endif

#define MEM_ALIGNMENT 64
#define MAX_SYMBOLS 64
size_t gl_packetLength = PACKET_LENGTH;

/* USB_VSC*/
#ifdef CONFIG_XLINK_USB_VSC
#include <usb_vsc.h>
const void* gl_AdrTbl = &VSCCompAdrTbl;
char gl_deviceNameWr[MAX_SYMBOLS] = USB_VSC_DEVNAME0;
char gl_deviceNameRd[MAX_SYMBOLS] = USB_VSC_DEVNAME0;
size_t usb_vsc_packet_length = PACKET_LENGTH;
#endif

/* USB_CDC*/
#ifdef CONFIG_XLINK_USB_CDC
#include <usb_uart.h>
const void* gl_AdrTbl = &UsbUartAdrTbl;
char gl_deviceNameWr[MAX_SYMBOLS] = "/dev/usb0";
char gl_deviceNameRd[MAX_SYMBOLS] = "/dev/usb1";
size_t usb_cdc_packet_length = PACKET_LENGTH;
#endif

/* PCIE_EP*/
#if defined(CONFIG_XLINK_PCIE) && defined(MA2480)
#include "OsDrvPcieSerial.h"
#define XLINK_PCIE_INTERFACE 0
size_t pcie_packet_length = PACKET_LENGTH;
#endif

#ifndef XLINK_PCIE_DATA_TIMEOUT
#define XLINK_PCIE_DATA_TIMEOUT 1000
#endif

#ifdef USE_TCP_IP
#include <sys/socket.h>
#include <netinet/in.h>

#define TCP_SOCKET_PORT 11490

size_t tcp_ip_packet_length = PACKET_LENGTH; // 1024
#endif /* USE_TCP_IP */

int xlink_allow_soft_reset = 1;

// Capability to specify occasional WD bump
static volatile int watchdog_timeout_ms = 0;

static int gl_fdWrite = -1, gl_fdRead = -1;

// ------------------------------------
// Weak overridable functions. Begin.
// ------------------------------------
void __attribute__((weak)) XLinkPlatformCloseRemoteReset(XLinkProtocol_t protocol) {
    // // Reset Myriad device if using USB or Ethernet
    // if(protocol == X_LINK_USB_VSC || protocol == X_LINK_TCP_IP) {
    //     if (xlink_allow_soft_reset) {
    //         SET_REG_WORD(CPR_MAS_RESET_ADR, 0);
    //     } else {
    //         printf("%s: Ignoring soft reset request. Halting!\n", __func__);
    //         asm("ta 1");
    //     }
    // }
}

void __attribute__((weak)) XLinkPlatformWatchdogKeepalive() {
    // Default does nothing
}

// ------------------------------------
// Weak overridable functions. End.
// ------------------------------------

// ------------------------------------
// Helpers. Begin.
// ------------------------------------

/*Acknowledge functions need to reside inside each protocols IOdevice.
This is a temporary solution below: (these need to be moved to USB_CDC component)*/
int acknowledgeDataWrite()
{
#ifdef CONFIG_XLINK_USB_CDC
    int nread = 0;
    uint8_t acknowledge;
    while(nread < (int)sizeof(acknowledge))
    {
        int count = read(gl_fdRead, &acknowledge, sizeof(acknowledge));
        if (count > 0)
            nread += count;
    }
    if(nread != sizeof(acknowledge) || acknowledge != 0xEF){
        printf("No acknowledge received %d %d\n", nread, acknowledge);
        return -1;      //error
    }
#endif
    return 0;
}

int acknowledgeDataRead(int param)
{
#ifdef CONFIG_XLINK_USB_CDC
    uint8_t ack = 0xEF;
    int nwrite = write(gl_fdWrite, &ack, sizeof(ack));
    if(nwrite != sizeof(ack))
    {
        printf("Failed to write data %d != %d\n", nwrite, param);
    }
#endif
    return param;
}


static inline void watchdogKeepaliveHelper(int wdTimeoutMs){
    /*
    // Init to zero
    static struct timespec prevTime = {0};

    // watchdog_timeout_ms being set
    if(wdTimeoutMs > 0){
        struct timespec diff, currentTime;
        MvGetTimestamp(&currentTime);
        MvTimespecDiff(&diff, &currentTime, &prevTime);
        int diffMs = diff.tv_sec * 10e3 + diff.tv_nsec / 10e6;
        if(diffMs >= wdTimeoutMs / 2){
            XLinkPlatformWatchdogKeepalive();
            prevTime = currentTime;
        }
    }
    */

}

// ------------------------------------
// Helpers. End.
// ------------------------------------

// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

void XLinkPlatformSetWatchdogTimeout(int ms){
    watchdog_timeout_ms = ms;
}


static XLinkProtocol_t protocol;

xLinkPlatformErrorCode_t XLinkPlatformInit(XLinkGlobalHandler_t* globalHandler)
{
    protocol = globalHandler->protocol;

    if(globalHandler->protocol == X_LINK_PCIE){
        #if defined(CONFIG_XLINK_PCIE)

        OsDrvPcieSerialInit();
        while(!OsDrvPcieSerialHostIsUp())
            continue;
        OsDrvPcieSerialOpen(XLINK_PCIE_INTERFACE);

        gl_packetLength = pcie_packet_length;

        #else
        assert(0 && "Selected incompatible option, compile with XLINK_USB_VSC set");
        #endif


    } else if(globalHandler->protocol == X_LINK_TCP_IP) {

        #if defined(USE_TCP_IP)

        c_bind_startDeviceDiscoveryService(3);

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0)
        {
            perror("socket");
            close(sockfd);
        }

        int reuse_addr = 1;
        int sc;
        sc = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int));
        if(sc < 0)
        {
            perror("setsockopt");
            close(sockfd);
        }

        struct sockaddr_in serv_addr = {}, client = {};
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(TCP_SOCKET_PORT);
        if(bind(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
        {
            perror("bind");
            close(sockfd);
        }

        if(listen(sockfd, 10) < 0)
        {
            perror("listen");
            close(sockfd);
        }

        unsigned len = sizeof(client);
        int connfd = accept(sockfd, (struct sockaddr*) &client, &len);
        if(connfd < 0)
        {
            perror("accept");
        }

        printf("listen acceptd\n");

        gl_fdRead = gl_fdWrite = connfd;
        gl_packetLength = tcp_ip_packet_length;

        #else
        assert(0 && "Selected incompatible option, compile with XLINK_USB_VSC set");
        #endif


    } else {
        // Defaulting to USB
        #if defined(CONFIG_XLINK_USB_CDC) || defined(CONFIG_XLINK_USB_VSC)     /*USB CLASSES*/

        rtems_status_code sc = rtems_io_register_driver(0, gl_AdrTbl, &gl_devMajor);
        if (sc != RTEMS_SUCCESSFUL) {
            printf("Driver Register failed !!!!\n");
            rtems_fatal_error_occurred(sc);
        }

        gl_fdWrite = open(gl_deviceNameWr, O_RDWR);
        gl_fdRead = open(gl_deviceNameRd, O_RDWR);
        if((gl_fdWrite < 0 || gl_fdRead < 0)) {
            printf("No device opened !!!! descriptors: %d %d\n", gl_fdWrite, gl_fdRead);
        }

        gl_packetLength = usb_vsc_packet_length;
        protocol = X_LINK_USB_VSC;

        #else
        assert(0 && "Selected incompatible option, compile with XLINK_USB_VSC set");
        #endif

    }

    return 0;
}


int XLinkPlatformCloseRemote(xLinkDeviceHandle_t* deviceHandle)
{
    UNUSED(deviceHandle);

    if(protocol == X_LINK_TCP_IP){
#ifdef USE_TCP_IP
        shutdown(gl_fdWrite, SHUT_RDWR);
#endif
    }
    close(gl_fdWrite);
    if(gl_fdRead != gl_fdWrite) {
        if(protocol == X_LINK_TCP_IP){
#ifdef USE_TCP_IP
            shutdown(gl_fdRead, SHUT_RDWR);
#endif
        }
        close(gl_fdRead);
    }

    if(protocol == X_LINK_TCP_IP) {
        if(gl_fdRead != -1 || gl_fdWrite != -1){
            usleep(200*1000);
        }
    }

    return 0;
}

int XLinkPlatformWrite2(xLinkDeviceHandle_t* deviceHandle, void* data, int totalSize, void* data2, int data2Size)
{
#ifdef CONFIG_XLINK_USB_CDC
assert(!"need to send in specified chunks before reading acknowledge");
#endif

    UNUSED(deviceHandle);
    UNUSED(data);
    UNUSED(totalSize);
    UNUSED(data2);
    UNUSED(data2Size);


#if defined(CONFIG_XLINK_USB_VSC) || defined(USE_TCP_IP)

    int errorCode = 0;
    void *dataToWrite[] = {data, data2, NULL};
    int sizeToWrite[] = {totalSize - data2Size, data2Size, 0};

    int writtenByteCount = 0, toWrite = 0, rc = 0;

    int totalSizeToWrite = 0;

    int pktlen = gl_packetLength; // May be modified externally, make a copy

    int copyWdTimeoutMs = watchdog_timeout_ms; // Make a non-volatile copy

    // restriction on the output data size
    // mitigates kernel crash on RPI when USB is used
    const int xlinkPacketSizeMultiply = deviceHandle->protocol == X_LINK_USB_VSC ? 1024 : 1; //for usb3, usb2 is 512
    // uint8_t *swapSpace = (uint8_t *)XLinkPlatformAllocateData(xlinkPacketSizeMultiply, 64);
    uint8_t swapSpaceScratchBuffer[xlinkPacketSizeMultiply + 64];
    uint8_t *swapSpace = ALIGN_UP((uint8_t*)swapSpaceScratchBuffer, 64);

    // the amount of bytes written from split transfer for "next" packet
    int previousSplitWriteSize = 0;
    for (int i = 0;; i++) {
        void *currentPacket = dataToWrite[i];
        int currentPacketSize = sizeToWrite[i];
        if (currentPacket == NULL) break;
        if (currentPacketSize == 0) break;
        // printf("currentPacket %p size %d \n", currentPacket, currentPacketSize);
        void *nextPacket = dataToWrite[i + 1];
        int nextPacketSize = sizeToWrite[i + 1];
        bool shouldSplitData = false;

        if (nextPacket != NULL && nextPacketSize > 0) {
            totalSizeToWrite += currentPacketSize - (currentPacketSize % xlinkPacketSizeMultiply);
            if(currentPacketSize % xlinkPacketSizeMultiply) {
                shouldSplitData = true;
            }
        } else {
            totalSizeToWrite += currentPacketSize;
        }

        // printf("writtenByteCount %d %d\n",writtenByteCount , totalSizeToWrite);
        int byteCountRelativeOffset = writtenByteCount;
        while (writtenByteCount < totalSizeToWrite) {
            toWrite = (pktlen && (totalSizeToWrite - writtenByteCount) > pktlen)
                          ? pktlen
                          : (totalSizeToWrite - writtenByteCount);
            rc = write(gl_fdRead, &((char *)currentPacket)[writtenByteCount - byteCountRelativeOffset + previousSplitWriteSize], toWrite);
            if (rc < 0)
            {
                errorCode = rc;
                goto function_epilogue;
            }
            writtenByteCount += rc;
            // printf("%s wrote %d \n", __FUNCTION__, rc);
            // acknowledgeDataWrite();      //temporary

            // Optional WD Bump helper
            if(rc > 0) {
                watchdogKeepaliveHelper(copyWdTimeoutMs);
            }

        }
        if (shouldSplitData) {
            int remainingToWriteCurrent = currentPacketSize - (totalSizeToWrite - byteCountRelativeOffset);
            // printf("remainingToWriteCurrent %d \n", remainingToWriteCurrent);
            if(remainingToWriteCurrent < 0 || remainingToWriteCurrent > xlinkPacketSizeMultiply) assert(0);
            int remainingToWriteNext = nextPacketSize > xlinkPacketSizeMultiply - remainingToWriteCurrent ? xlinkPacketSizeMultiply - remainingToWriteCurrent : nextPacketSize;
            // printf("remainingToWriteNext %d \n", remainingToWriteNext);
            if(remainingToWriteNext < 0 || remainingToWriteNext > xlinkPacketSizeMultiply) assert(0);

            if (remainingToWriteCurrent) {
                memcpy(swapSpace, &((char *)currentPacket)[writtenByteCount - byteCountRelativeOffset + previousSplitWriteSize], remainingToWriteCurrent);
                if(remainingToWriteNext) {
                    memcpy(swapSpace + remainingToWriteCurrent, nextPacket, remainingToWriteNext);
                }
                toWrite = remainingToWriteCurrent + remainingToWriteNext;
                if(toWrite > xlinkPacketSizeMultiply) assert(0);
                rc = write(gl_fdRead, swapSpace, toWrite);
                if (rc < 0)
                {
                    errorCode = rc;
                    goto function_epilogue;
                }
                writtenByteCount += rc;
                totalSizeToWrite += remainingToWriteCurrent;
                // printf("%s wrote %d \n", __FUNCTION__, rc);

                previousSplitWriteSize = remainingToWriteNext;
            }
        } else {
            previousSplitWriteSize = 0;
        }
    }

function_epilogue:
    // XLinkPlatformDeallocateData(swapSpace, xlinkPacketSizeMultiply, 64);
    if (errorCode) return errorCode;

    return writtenByteCount;

#elif defined(CONFIG_XLINK_PCIE)
    UNUSED(toWrite);
    assert(!"not implemented");
    int byteCount = 0, toWrite = 0, rc = 0;

    while (byteCount < size){
        rc = OsDrvPcieSerialWrite(XLINK_PCIE_INTERFACE, data + byteCount,
                                  size - byteCount, XLINK_PCIE_DATA_TIMEOUT);

        if (rc < 0) {
            rtems_fatal_error_occurred(-rc);
        }

        // Optional WD Bump helper
        if(rc > 0) {
            watchdogKeepaliveHelper(copyWdTimeoutMs);
        }

        byteCount += rc;
    }
    return byteCount;

#endif
    return 0;
}


int XLinkPlatformWrite(xLinkDeviceHandle_t* deviceHandle, void* data, int size)
{
    UNUSED(deviceHandle);
    UNUSED(data);
    UNUSED(size);

    int byteCount = 0, toWrite = 0, rc = 0;
    int copyWdTimeoutMs = watchdog_timeout_ms; // Make a non-volatile copy

#if defined(CONFIG_XLINK_USB_CDC) || defined(CONFIG_XLINK_USB_VSC) || defined(USE_TCP_IP)
    size_t pktlen = gl_packetLength; // May be modified externally, make a copy
    while (byteCount < size){
        toWrite = (pktlen && (size_t)(size - byteCount) > pktlen) \
                    ? pktlen : (size_t)(size - byteCount);
        rc = write(gl_fdRead, &((char*)data)[byteCount], toWrite);
        if (rc < 0) {
            return rc;
        }
        byteCount += rc;
        acknowledgeDataWrite();      //temporary

        // Optional WD Bump helper
        if(rc > 0) {
            watchdogKeepaliveHelper(copyWdTimeoutMs);
        }

    }
#elif defined(CONFIG_XLINK_PCIE)
    UNUSED(toWrite);
    while (byteCount < size){
        rc = OsDrvPcieSerialWrite(XLINK_PCIE_INTERFACE, data + byteCount,
                                  size - byteCount, XLINK_PCIE_DATA_TIMEOUT);

        if (rc < 0) {
            rtems_fatal_error_occurred(-rc);
        }

        // Optional WD Bump helper
        if(rc > 0) {
            watchdogKeepaliveHelper(copyWdTimeoutMs);
        }

        byteCount += rc;
    }
#endif
    return byteCount;
}

int XLinkPlatformRead(xLinkDeviceHandle_t* deviceHandle, void* data, int size)
{
    UNUSED(deviceHandle);
    UNUSED(data);
    UNUSED(size);

    int byteCount = 0, toRead = 0, rc = 0;
    int copyWdTimeoutMs = watchdog_timeout_ms; // Make a non-volatile copy

#if defined(CONFIG_XLINK_USB_CDC) || defined(CONFIG_XLINK_USB_VSC) || defined(USE_TCP_IP)
    size_t pktlen = gl_packetLength; // May be modified externally, make a copy
    while(byteCount < size) {
        toRead = (pktlen && (size_t)(size - byteCount) > pktlen) \
                    ? pktlen : (size_t)(size - byteCount);
        rc = read(gl_fdWrite, &((char*)data)[byteCount], toRead);
        if (rc < 0) {
            continue;
        }
        byteCount += rc;
        acknowledgeDataRead(size);   //temporary

        // Optional WD Bump helper
        if(rc > 0) {
            watchdogKeepaliveHelper(copyWdTimeoutMs);
        }

    }
#elif defined(CONFIG_XLINK_PCIE)
    UNUSED(toRead);
    while(byteCount < size) {
        rc = OsDrvPcieSerialRead(XLINK_PCIE_INTERFACE, data + byteCount,
                                 size - byteCount, XLINK_PCIE_DATA_TIMEOUT);
        if (rc < 0) {
           rtems_fatal_error_occurred(-rc);
        }

        // Optional WD Bump helper
        if(rc > 0) {
            watchdogKeepaliveHelper(copyWdTimeoutMs);
        }

        byteCount += rc;
    }
#endif
    return byteCount;
}

void* XLinkPlatformAllocateData(uint32_t size, uint32_t alignment)
{
    // return malloc(size);
    void* ret = NULL;
    posix_memalign(&ret, alignment, size);
    return ret;
/*
    assert(MEM_ALIGNMENT % alignment == 0);
    void* ret = NULL;
#ifdef XLINK_USE_MEMORY_MANAGER
    # ifdef MEMORY_MANAGER_THRESHOLD_XLINK
    if (size > MEMORY_MANAGER_THRESHOLD_XLINK)
# endif
    {
        ret = MemMgrAlloc(size, DDR_AREA, alignment);
    }
# ifdef MEMORY_MANAGER_THRESHOLD_XLINK
    if (size <= MEMORY_MANAGER_THRESHOLD_XLINK)
    {
        posix_memalign(&ret, alignment, size);
    }
# endif
#else  // MEMORY_MANAGER_THRESHOLD_XLINK
    if (posix_memalign(&ret, alignment, size) != 0)
        ret = NULL;
#endif
    if (ret) {
        rtems_cache_invalidate_multiple_data_lines(ret, size);
    }
    return ret;
*/
}

void XLinkPlatformDeallocateData(void* ptr, uint32_t size, uint32_t alignment)
{
    UNUSED(alignment);
    if (!ptr)
        return;
#ifdef XLINK_USE_MEMORY_MANAGER
# ifdef MEMORY_MANAGER_THRESHOLD_XLINK
    if (size > MEMORY_MANAGER_THRESHOLD_XLINK)
# endif
    {
        MemMgrFree(ptr);
    }
// if threshold not defined - all memory allocated by MemoryManager
# ifdef MEMORY_MANAGER_THRESHOLD_XLINK
    if (size <= MEMORY_MANAGER_THRESHOLD_XLINK)
    {
        free(ptr);
    }
# endif

#else  // XLINK_USE_MEMORY_MANAGER
    UNUSED(size);
    free(ptr);
#endif  // XLINK_USE_MEMORY_MANAGER
}

// ------------------------------------
// XLinkPlatform API implementation. End.
// ------------------------------------

