// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "usb_mx_id.h"
#if (defined(_WIN32) || defined(_WIN64) )
#include "win_usb.h"
#include "win_time.h"
#include "win_pthread.h"
#else
#include <libusb.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#endif
#include "usb_boot.h"
#include "XLinkStringUtils.h"
#include "XLinkPublicDefines.h"

#define DEFAULT_VID                 0x03E7

#define DEFAULT_WRITE_TIMEOUT       2000
#define DEFAULT_CONNECT_TIMEOUT     20000
#define DEFAULT_SEND_FILE_TIMEOUT   10000
#define USB1_CHUNKSZ                64

#define MVLOG_UNIT_NAME xLinkUsb
#include "XLinkLog.h"

/*
 * ADDRESS_BUFF_SIZE 35 = 4*7+7.
 * '255-' x 7 (also gives us nul-terminator for last entry)
 * 7 => to add "-maXXXX"
 */
#define ADDRESS_BUFF_SIZE           35

#define OPEN_DEV_ERROR_MESSAGE_LENGTH 128

static unsigned int bulk_chunklen = DEFAULT_CHUNKSZ;
static int write_timeout = DEFAULT_WRITE_TIMEOUT;
static int connect_timeout = DEFAULT_CONNECT_TIMEOUT;
static int initialized;

static int MX_ID_TIMEOUT = 100;


typedef struct {
    int pid;
    char name[16];
} deviceBootInfo_t;

static deviceBootInfo_t supportedDevices[] = {
#if 0 // Myriad 2 device has been deprecated since 2020.4 release
    {
        .pid = 0x2150,
        .name = "ma2450"
    },
#endif
    {
        .pid = 0x2485,
        .name = "ma2480"
    },
    {
        //To support the case where the port name change, or it's already booted
        .pid = DEFAULT_OPENPID,
        .name = ""
    },
    {
        .pid = DEFAULT_BOOTLOADER_PID,
        .name = "bootloader"
    },
    {
        .pid = DEFAULT_FLASH_BOOTED_PID,
        .name = "flash_booted"
    }
};


// for now we'll only use the loglevel for usb boot. can bring it into
// the rest of usblink later
// use same levels as mvnc_loglevel for now
#if (defined(_WIN32) || defined(_WIN64) )
void initialize_usb_boot()
{
    if (initialized == 0)
    {
        usb_init();
    }
    // We sanitize the situation by trying to reset the devices that have been left open
    initialized = 1;
}
#else
void __attribute__((constructor)) usb_library_load()
{
    //MVLOGLEVEL(MVLOG_UNIT_NAME) = MVLOG_DEBUG;
    initialized = !libusb_init(NULL);
}

void __attribute__((destructor)) usb_library_unload()
{
    if(initialized)
        libusb_exit(NULL);
}
#endif

typedef struct timespec highres_time_t;

static inline void highres_gettime(highres_time_t *ptr) {
    clock_gettime(CLOCK_REALTIME, ptr);
}

static inline double highres_elapsed_ms(highres_time_t *start, highres_time_t *end) {
    struct timespec temp;
    if((end->tv_nsec - start->tv_nsec) < 0) {
        temp.tv_sec = end->tv_sec - start->tv_sec - 1;
        temp.tv_nsec = 1000000000 + end->tv_nsec-start->tv_nsec;
    } else {
        temp.tv_sec = end->tv_sec - start->tv_sec;
        temp.tv_nsec = end->tv_nsec - start->tv_nsec;
    }
    return (double)(temp.tv_sec * 1000) + (((double)temp.tv_nsec) * 0.000001);
}

static const char *get_pid_name(int pid)
{
    int n = sizeof(supportedDevices)/sizeof(supportedDevices[0]);
    int i;

    for (i = 0; i < n; i++)
    {
        if (supportedDevices[i].pid == pid)
            return supportedDevices[i].name;
    }

    return NULL;
}

const char * usb_get_pid_name(int pid)
{
    return get_pid_name(pid);
}

int get_pid_by_name(const char* name)
{
    char* p = strchr(name, '-');
    if (p == NULL) {
        mvLog(MVLOG_DEBUG, "Device name (%s) not supported", name);
        return -1;
    }
    p++; //advance to point to the name
    int i;
    int n = sizeof(supportedDevices)/sizeof(supportedDevices[0]);

    for (i = 0; i < n; i++)
    {
        if (strcmp(supportedDevices[i].name, p) == 0)
            return supportedDevices[i].pid;
    }
    return -1;
}

static int is_pid_supported(int pid)
{
    int n = sizeof(supportedDevices)/sizeof(supportedDevices[0]);
    int i;
    for (i = 0; i < n; i++) {
        if (supportedDevices[i].pid == pid)
            return 1;
    }
    return 0;
}

int isMyriadDevice(const int idVendor, const int idProduct) {
    // Device is Myriad and pid supported
    if (idVendor == DEFAULT_VID && is_pid_supported(idProduct) == 1)
        return 1;
    // Device is Myriad and device booted
    if (idVendor == DEFAULT_OPENVID && idProduct == DEFAULT_OPENPID)
        return 1;
    // Device is Myriad and in bootloader
    if (idVendor == DEFAULT_OPENVID && idProduct == DEFAULT_BOOTLOADER_PID)
        return 1;
    // Device is Myriad and in flash booted state
    if (idVendor == DEFAULT_OPENVID && idProduct == DEFAULT_FLASH_BOOTED_PID)
        return 1;
    return 0;
}

int isBootedMyriadDevice(const int idVendor, const int idProduct) {
    // Device is Myriad, booted device pid
    if (idVendor == DEFAULT_VID && idProduct == DEFAULT_OPENPID) {
        return 1;
    }
    return 0;
}

int isBootloaderMyriadDevice(const int idVendor, const int idProduct) {
    // Device is Myriad and in bootloader
    if (idVendor == DEFAULT_OPENVID && idProduct == DEFAULT_BOOTLOADER_PID)
        return 1;
    return 0;
}

int isFlashBootedMyriadDevice(const int idVendor, const int idProduct) {
    // Device is Myriad and in bootloader
    if (idVendor == DEFAULT_OPENVID && idProduct == DEFAULT_FLASH_BOOTED_PID)
        return 1;
    return 0;
}

int isNotBootedMyriadDevice(const int idVendor, const int idProduct) {
    // Device is Myriad, pid supported and it's is not booted device
    if (idVendor == DEFAULT_VID && is_pid_supported(idProduct) == 1
        && idProduct != DEFAULT_OPENPID && idProduct != DEFAULT_BOOTLOADER_PID && idProduct != DEFAULT_FLASH_BOOTED_PID) {
        return 1;
    }
    return 0;
}



#if (!defined(_WIN32) && !defined(_WIN64) )

static double steady_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}


static double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}
static const char* gen_addr_compat(libusb_device *dev, int pid, bool use_bus){

    static char buff[ADDRESS_BUFF_SIZE];
    uint8_t pnums[7];
    int pnum_cnt, i;
    char *p;

    pnum_cnt = libusb_get_port_numbers(dev, pnums, 7);
    if (pnum_cnt == LIBUSB_ERROR_OVERFLOW) {
        // shouldn't happen!
        mv_strcpy(buff, ADDRESS_BUFF_SIZE, "<error>");
        return buff;
    }
    p = buff;

    if(use_bus){
        uint8_t bus = libusb_get_bus_number(dev);
        p += snprintf(p, sizeof(buff), "%u.", bus);
    }

    for (i = 0; i < pnum_cnt - 1; i++)
        p += snprintf(p, sizeof(buff),"%u.", pnums[i]);

    p += snprintf(p, sizeof(buff),"%u", pnums[i]);
    const char* dev_name = get_pid_name(pid);

    if (dev_name != NULL) {
        snprintf(p, sizeof(buff),"-%s", dev_name);
    } else {
        mv_strcpy(buff, ADDRESS_BUFF_SIZE,"<error>");
        return buff;
    }

    return buff;

}


static const char *gen_addr(struct libusb_device_descriptor* pDesc, libusb_device *dev, int pid)
{

#ifdef XLINK_USE_MX_ID_NAME

    usb_mx_id_cache_init();

    // Static variables
    static char final_addr[XLINK_MAX_NAME_SIZE];
    static char mx_id[XLINK_MAX_MX_ID_SIZE];

    // Set final_addr as error first
    strncpy(final_addr, "<error>", sizeof(final_addr));

    // generate unique (full) usb bus-port path
    const char* compat_addr = gen_addr_compat(dev, pid, true);

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(compat_addr, mx_id);

    if(found){
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mx_id);

    } else {
        // If not found, retrieve mx_id

        // get serial from usb descriptor
        libusb_device_handle *handle = NULL;
        int libusb_rc = 0;

        // Open device
        libusb_rc = libusb_open(dev, &handle);
        if (libusb_rc < 0){
            // Some kind of error, either NO_MEM, ACCESS, NO_DEVICE or other
            // In all these cases, return
            // no cleanup needed
            return final_addr;
        }


        // Retry getting MX ID for 5ms
        const double RETRY_TIMEOUT = 0.005; // 5ms
        const int SLEEP_BETWEEN_RETRIES_USEC = 100; // 100us
        double t_retry = seconds();
        do {

            // if UNBOOTED state, perform mx_id retrieval procedure using small program and a read command
            if(pid == DEFAULT_UNBOOTPID_2485 || pid == DEFAULT_UNBOOTPID_2150){

                // Get configuration first (From OS cache)
                int active_configuration = -1;
                if( (libusb_rc = libusb_get_configuration(handle, &active_configuration)) == 0){
                    if(active_configuration != 1){
                        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
                        if ((libusb_rc = libusb_set_configuration(handle, 1)) < 0) {
                            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", libusb_strerror(libusb_rc));

                            // retry
                            usleep(SLEEP_BETWEEN_RETRIES_USEC);
                            continue;
                        }
                    }
                } else {
                    // getting config failed...
                    mvLog(MVLOG_ERROR, "libusb_set_configuration: %s", libusb_strerror(libusb_rc));

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }


                // Claim interface (as we'll be doing IO on endpoints)
                if ((libusb_rc = libusb_claim_interface(handle, 0)) < 0) {
                    if(libusb_rc != LIBUSB_ERROR_BUSY){
                        mvLog(MVLOG_ERROR, "libusb_claim_interface: %s", libusb_strerror(libusb_rc));
                    }
                    // retry - most likely device busy by another app
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }


                const int send_ep = 0x01;
                const int size = usb_mx_id_get_payload_size();
                int transferred = 0;
                if ((libusb_rc = libusb_bulk_transfer(handle, send_ep, ((uint8_t*) usb_mx_id_get_payload()), size, &transferred, MX_ID_TIMEOUT)) < 0) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer send: %s", libusb_strerror(libusb_rc));

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }
                // Transfer as mxid_read_cmd size is less than 512B it should transfer all
                if (size != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer written %d, expected %d", transferred, size);

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }

                const int recv_ep = 0x81;
                const int expected = 9;
                uint8_t rbuf[128];
                transferred = 0;
                if ((libusb_rc = libusb_bulk_transfer(handle, recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT)) < 0) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer recv: %s", libusb_strerror(libusb_rc));

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }
                if (expected != transferred) {
                    mvLog(MVLOG_ERROR, "libusb_bulk_transfer read %d, expected %d", transferred, expected);

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }

                // Release claimed interface
                // ignore error as it doesn't matter
                libusb_release_interface(handle, 0);

                // Parse mx_id into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < transferred; i++) {
                    sprintf(mx_id + 2*i, "%02X", rbuf[i]);
                }

                // Indicate no error
                libusb_rc = 0;

            } else {

                if( (libusb_rc = libusb_get_string_descriptor_ascii(handle, pDesc->iSerialNumber, ((uint8_t*) mx_id), sizeof(mx_id))) < 0){
                    mvLog(MVLOG_WARN, "Failed to get string descriptor");

                    // retry
                    usleep(SLEEP_BETWEEN_RETRIES_USEC);
                    continue;
                }

                // Indicate no error
                libusb_rc = 0;

            }

        } while (libusb_rc != 0 && seconds() - t_retry < RETRY_TIMEOUT);

        // Close opened device
        libusb_close(handle);


        // if mx_id couldn't be retrieved, exit by returning final_addr ("<error>")
        if(libusb_rc != 0){
            return final_addr;
        }

        // Cache the retrieved mx_id
        // Find empty space and store this entry
        // If no empty space, don't cache (possible case: >16 devices)
        int cache_index = usb_mx_id_cache_store_entry(mx_id, compat_addr);
        if(cache_index >= 0){
            // debug print
            mvLog(MVLOG_DEBUG, "Cached MX ID %s at index %d", mx_id, cache_index);
        } else {
            // debug print
            mvLog(MVLOG_DEBUG, "Couldn't cache MX ID %s", mx_id);
        }

    }

    // At the end add dev_name to retain compatibility with rest of the codebase
    const char* dev_name = get_pid_name(pid);

    // Create address [mx_id]-[dev_name]
    snprintf(final_addr, sizeof(final_addr), "%s-%s", mx_id, dev_name);

    mvLog(MVLOG_DEBUG, "Returning generated name: %s", final_addr);

    return final_addr;

#else

    // return compatible addr
    return gen_addr_compat(dev, pid, false);

#endif

}

static pthread_mutex_t globalMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Find usb device address
 * @param input_addr  Device name (address) which would be returned. If not empty, we will try to
 *                  find device with this name
 *
 * @details
 * Find any device (device = 0):
 * <br> 1) Any myriad device:                    vid = AUTO_VID & pid = AUTO_PID
 * <br> 2) Any not booted myriad device:         vid = AUTO_VID & pid = AUTO_UNBOOTED_PID
 * <br> 3) Any booted myriad device:             vid = AUTO_VID & pid = DEFAULT_OPENPID
 * <br> 4) Specific Myriad 2 or Myriad X device: vid = AUTO_VID & pid = DEFAULT_UNBOOTPID_2485 or DEFAULT_UNBOOTPID_2150
 * <br><br> Find specific device (device != 0):
 * <br> device arg should be not null, search by addr (name) and return device struct
 *
 * @note
 * Index can be used to iterate through all connected myriad devices and save their names.
 * It will loop only over suitable devices specified by vid and pid
 */
usbBootError_t usb_find_device_with_bcd(unsigned idx, char *input_addr,
                                        unsigned addrsize, void **device, int vid, int pid, uint16_t* bcdusb) {
    if (pthread_mutex_lock(&globalMutex)) {
        mvLog(MVLOG_ERROR, "globalMutex lock failed");
        return USB_BOOT_ERROR;
    }
    int searchByName = 0;
    static libusb_device **devs = NULL;
    libusb_device *dev = NULL;
    struct libusb_device_descriptor desc;
    int count = 0;
    size_t i;
    int res;

    if (!initialized) {
        mvLog(MVLOG_ERROR, "Library has not been initialized when loaded");
        if (pthread_mutex_unlock(&globalMutex)) {
            mvLog(MVLOG_ERROR, "globalMutex unlock failed");
        }
        return USB_BOOT_ERROR;
    }

    if (strlen(input_addr) > 1) {
        searchByName = 1;
    }

    // Update device list if empty or if indx 0
    if (!devs || idx == 0) {
        if (devs) {
            libusb_free_device_list(devs, 1);
            devs = 0;
        }
        if ((res = libusb_get_device_list(NULL, &devs)) < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", libusb_strerror(res));
            if (pthread_mutex_unlock(&globalMutex)) {
                mvLog(MVLOG_ERROR, "globalMutex unlock failed");
            }
            return USB_BOOT_ERROR;
        }
    }

    // Loop over all usb devices, increase count only if myriad device
    i = 0;
    while ((dev = devs[i++]) != NULL) {
        if ((res = libusb_get_device_descriptor(dev, &desc)) < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device descriptor: %s", libusb_strerror(res));
            continue;
        }

        // If found device have the same id and vid as input
        if ( (desc.idVendor == vid && desc.idProduct == pid)
             // Any myriad device
             || (vid == AUTO_VID && pid == AUTO_PID
                 && isMyriadDevice(desc.idVendor, desc.idProduct))
             // Any not booted myriad device
             || (vid == AUTO_VID && (pid == AUTO_UNBOOTED_PID)
                 && isNotBootedMyriadDevice(desc.idVendor, desc.idProduct))
             // Any not booted with specific pid
             || (vid == AUTO_VID && pid == desc.idProduct
                 && isNotBootedMyriadDevice(desc.idVendor, desc.idProduct))
             // Any booted device
             || (vid == AUTO_VID && pid == DEFAULT_OPENPID
                 && isBootedMyriadDevice(desc.idVendor, desc.idProduct))
             // Any bootloader device
             || (vid == AUTO_VID && pid == DEFAULT_BOOTLOADER_PID
                 && isBootloaderMyriadDevice(desc.idVendor, desc.idProduct))
             // Any flash booted device
             || (vid == AUTO_VID && pid == DEFAULT_FLASH_BOOTED_PID
                 && isFlashBootedMyriadDevice(desc.idVendor, desc.idProduct))
        ) {
            if (device) {
                const char *dev_addr = gen_addr(&desc, dev, get_pid_by_name(input_addr));
                if (!strcmp(dev_addr, input_addr)) {
#if 0 // To avoid spam in Debug mode
                    mvLog(MVLOG_DEBUG, "Found Address: %s - VID/PID %04x:%04x",
                          input_addr, desc.idVendor, desc.idProduct);
#endif

                    libusb_ref_device(dev);
                    libusb_free_device_list(devs, 1);
                    if (bcdusb)
                        *bcdusb = desc.bcdUSB;
                    *device = dev;
                    devs = 0;

                    if (pthread_mutex_unlock(&globalMutex)) {
                        mvLog(MVLOG_ERROR, "globalMutex unlock failed");
                    }
                    return USB_BOOT_SUCCESS;
                }
            } else if (searchByName) {
                const char *dev_addr = gen_addr(&desc, dev, desc.idProduct);
                // If the same add as input
                if (!strcmp(dev_addr, input_addr)) {
#if 0 // To avoid spam in Debug mode
                    mvLog(MVLOG_DEBUG, "Found Address: %s - VID/PID %04x:%04x",
                          input_addr, desc.idVendor, desc.idProduct);
#endif

                    if (pthread_mutex_unlock(&globalMutex)) {
                        mvLog(MVLOG_ERROR, "globalMutex unlock failed");
                    }
                    return USB_BOOT_SUCCESS;
                }
            } else if ((int)idx == count) {
                const char *caddr = gen_addr(&desc, dev, desc.idProduct);
#if 0 // To avoid spam in Debug mode
                mvLog(MVLOG_DEBUG, "Device %d Address: %s - VID/PID %04x:%04x",
                      idx, caddr, desc.idVendor, desc.idProduct);
#endif
                mv_strncpy(input_addr, addrsize, caddr, addrsize - 1);
                if (pthread_mutex_unlock(&globalMutex)) {
                    mvLog(MVLOG_ERROR, "globalMutex unlock failed");
                }
                return USB_BOOT_SUCCESS;
            }
            count++;
        }
    }
    libusb_free_device_list(devs, 1);
    devs = 0;
    if (pthread_mutex_unlock(&globalMutex)) {
        mvLog(MVLOG_ERROR, "globalMutex unlock failed");
    }
    return USB_BOOT_DEVICE_NOT_FOUND;
}
#endif

#if (defined(_WIN32) || defined(_WIN64) )
usbBootError_t usb_find_device(unsigned idx, char *addr, unsigned addrsize, void **device, int vid, int pid)
{
    if (!addr)
        return USB_BOOT_ERROR;

    if(!initialized)
    {
        mvLog(MVLOG_ERROR, "Library has not been initialized when loaded");
        return USB_BOOT_ERROR;
    }

    return win_usb_find_device(idx, addr, addrsize, device, vid, pid);
}
#endif


#if (!defined(_WIN32) && !defined(_WIN64) )
static libusb_device_handle *usb_open_device(libusb_device *dev, uint8_t *endpoint, char *err_string_buff, int err_max_len)
{
    struct libusb_config_descriptor *cdesc;
    const struct libusb_interface_descriptor *ifdesc;
    libusb_device_handle *h = NULL;
    int res, i;

    if((res = libusb_open(dev, &h)) < 0)
    {
        snprintf(err_string_buff, err_max_len, "cannot open device: %s\n", libusb_strerror(res));
        return 0;
    }

    // Get configuration first
    int active_configuration = -1;
    if((res = libusb_get_configuration(h, &active_configuration)) < 0){
        snprintf(err_string_buff, err_max_len, "setting config 1 failed: %s\n", libusb_strerror(res));
        libusb_close(h);
        return 0;
    }

    // Check if set configuration call is needed
    if(active_configuration != 1){
        mvLog(MVLOG_DEBUG, "Setting configuration from %d to 1\n", active_configuration);
        if ((res = libusb_set_configuration(h, 1)) < 0) {
            mvLog(MVLOG_ERROR, "libusb_set_configuration: %s\n", libusb_strerror(res));
            snprintf(err_string_buff, err_max_len, "setting config 1 failed: %s\n", libusb_strerror(res));
            libusb_close(h);
            return 0;
        }
    }

    if((res = libusb_claim_interface(h, 0)) < 0)
    {
        snprintf(err_string_buff, err_max_len, "claiming interface 0 failed: %s\n", libusb_strerror(res));
        libusb_close(h);
        return 0;
    }
    if((res = libusb_get_config_descriptor(dev, 0, &cdesc)) < 0)
    {
        snprintf(err_string_buff, err_max_len, "Unable to get USB config descriptor: %s\n", libusb_strerror(res));
        libusb_close(h);
        return 0;
    }
    ifdesc = cdesc->interface->altsetting;
    for(i=0; i<ifdesc->bNumEndpoints; i++)
    {
        mvLog(MVLOG_DEBUG, "Found EP 0x%02x : max packet size is %u bytes",
              ifdesc->endpoint[i].bEndpointAddress, ifdesc->endpoint[i].wMaxPacketSize);
        if((ifdesc->endpoint[i].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if( !(ifdesc->endpoint[i].bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) )
        {
            *endpoint = ifdesc->endpoint[i].bEndpointAddress;
            bulk_chunklen = ifdesc->endpoint[i].wMaxPacketSize;
            libusb_free_config_descriptor(cdesc);
            return h;
        }
    }
    libusb_free_config_descriptor(cdesc);
    mv_strcpy(err_string_buff, OPEN_DEV_ERROR_MESSAGE_LENGTH,
              "Unable to find BULK OUT endpoint\n");
    libusb_close(h);
    return 0;
}
#endif
// timeout: -1 = no (infinite) timeout, 0 = must happen immediately


#if (!defined(_WIN32) && !defined(_WIN64) )
static int wait_findopen(const char *device_address, int timeout, libusb_device **dev, libusb_device_handle **devh, uint8_t *endpoint,uint16_t* bcdusb)
#else
static int wait_findopen(const char *device_address, int timeout, libusb_device **dev, libusb_device_handle **devh, uint8_t *endpoint)
#endif
{
    int i, rc;
    char last_open_dev_err[OPEN_DEV_ERROR_MESSAGE_LENGTH];
    double elapsedTime = 0;
    highres_time_t t1, t2;

    if (device_address == NULL) {
        return USB_BOOT_ERROR;
    }

    usleep(100000);

    if(timeout == -1){
        mvLog(MVLOG_DEBUG, "Starting wait for connect, no timeout");
    } else if(timeout == 0){
        mvLog(MVLOG_DEBUG, "Trying to connect");
    } else {
        mvLog(MVLOG_DEBUG, "Starting wait for connect with %ums timeout (device_address: %s)", timeout, device_address);
    }

    last_open_dev_err[0] = 0;
    i = 0;
    for(;;)
    {
        highres_gettime(&t1);
        int addr_size = (int)strlen(device_address);
#if (!defined(_WIN32) && !defined(_WIN64) )
        rc = usb_find_device_with_bcd(0, (char*)device_address, addr_size, (void**)dev,
                                      DEFAULT_VID, get_pid_by_name(device_address), bcdusb);
#else
        rc = usb_find_device(0, (char *)device_address, addr_size, (void **)dev,
            DEFAULT_VID, get_pid_by_name(device_address));
#endif
        if(rc < 0)
            return USB_BOOT_ERROR;
        if(!rc)
        {
#if (!defined(_WIN32) && !defined(_WIN64) )
            *devh = usb_open_device(*dev, endpoint, last_open_dev_err, OPEN_DEV_ERROR_MESSAGE_LENGTH);
#else
            *devh = usb_open_device(*dev, endpoint, 0, last_open_dev_err, OPEN_DEV_ERROR_MESSAGE_LENGTH);
#endif
            if(*devh != NULL)
            {
                mvLog(MVLOG_DEBUG, "Found and opened device");
                return 0;
            }
#if (!defined(_WIN32) && !defined(_WIN64) )
            libusb_unref_device(*dev);
            *dev = NULL;
#endif
        }
        highres_gettime(&t2);
        elapsedTime += highres_elapsed_ms(&t1, &t2);

        if(timeout != -1)
        {
            if(last_open_dev_err[0])
                mvLog(MVLOG_DEBUG, "Last opened device name: %s", last_open_dev_err);

            return rc ? USB_BOOT_DEVICE_NOT_FOUND : USB_BOOT_TIMEOUT;
        } else if (elapsedTime > (double)timeout) {
            return rc ? USB_BOOT_DEVICE_NOT_FOUND : USB_BOOT_TIMEOUT;
        }
        i++;
        usleep(100000);
    }
    return 0;
}

#if (!defined(_WIN32) && !defined(_WIN64) )
static int send_file(libusb_device_handle* h, uint8_t endpoint, const uint8_t* tx_buf, unsigned filesize,uint16_t bcdusb)
#else
static int send_file(libusb_device_handle *h, uint8_t endpoint, const uint8_t *tx_buf, unsigned filesize)
#endif
{
    const uint8_t *p;
    int rc;
    int wb, twb, wbr;
    double elapsedTime;
    highres_time_t t1, t2;
    int bulk_chunklen=DEFAULT_CHUNKSZ;
    elapsedTime = 0;
    twb = 0;
    p = tx_buf;
    int send_zlp = ((filesize % 512) == 0);

#if (!defined(_WIN32) && !defined(_WIN64) )
    if(bcdusb < 0x200) {
        bulk_chunklen = USB1_CHUNKSZ;
    }
#endif
    mvLog(MVLOG_DEBUG, "Performing bulk write of %u bytes...", filesize);
    while(((unsigned)twb < filesize) || send_zlp)
    {
        highres_gettime(&t1);
        wb = filesize - twb;
        if(wb > bulk_chunklen)
            wb = bulk_chunklen;
        wbr = 0;
#if (!defined(_WIN32) && !defined(_WIN64) )
        rc = libusb_bulk_transfer(h, endpoint, (void *)p, wb, &wbr, write_timeout);
#else
        rc = usb_bulk_write(h, endpoint, (void *)p, wb, &wbr, write_timeout);
#endif
        if((rc || (wb != wbr)) && (wb != 0)) // Don't check the return code for ZLP
        {
            if(rc == LIBUSB_ERROR_NO_DEVICE)
                break;
            mvLog(MVLOG_WARN, "bulk write: %s (%d bytes written, %d bytes to write)", libusb_strerror(rc), wbr, wb);
            if(rc == LIBUSB_ERROR_TIMEOUT)
                return USB_BOOT_TIMEOUT;
            else return USB_BOOT_ERROR;
        }
        highres_gettime(&t2);
        elapsedTime += highres_elapsed_ms(&t1, &t2);
        if (elapsedTime > DEFAULT_SEND_FILE_TIMEOUT) {
            return USB_BOOT_TIMEOUT;
        }
        if(wb == 0) // ZLP just sent, last packet
            break;
        twb += wbr;
        p += wbr;
    }

#ifndef NDEBUG
    double MBpS = ((double)filesize / 1048576.) / (elapsedTime * 0.001);
    mvLog(MVLOG_DEBUG, "Successfully sent %u bytes of data in %lf ms (%lf MB/s)", filesize, elapsedTime, MBpS);
#endif

    return 0;
}

int usb_boot(const char *addr, const void *mvcmd, unsigned size)
{
    int rc = 0;
    uint8_t endpoint;

#if (defined(_WIN32) || defined(_WIN64) )
    void *dev = NULL;
    struct _usb_han *h;

    rc = wait_findopen(addr, connect_timeout, &dev, &h, &endpoint);
    if(rc) {
        usb_close_device(h);
        usb_free_device(dev);
        return rc;
    }
    rc = send_file(h, endpoint, mvcmd, size);
    usb_close_device(h);
    usb_free_device(dev);
#else
    libusb_device *dev;
    libusb_device_handle *h;
    uint16_t bcdusb=-1;

    rc = wait_findopen(addr, connect_timeout, &dev, &h, &endpoint,&bcdusb);

    if(rc) {
        return rc;
    }
    rc = send_file(h, endpoint, mvcmd, size,bcdusb);
    if (h) {
        libusb_release_interface(h, 0);
        libusb_close(h);
    }
    if (dev) {
        libusb_unref_device(dev);
    }

#endif
    return rc;
}
