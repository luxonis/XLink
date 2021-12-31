// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
//#define _CRT_SECURE_NO_WARNINGS

#define INITGUID
#include <windows.h>
#include <winusb.h>
#include <usbiodef.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "win_usb.h"
#include "XLinkPublicDefines.h"
#include "usb_boot.h"
#include "usb_mx_id.h"
#include "stdbool.h"

#define MVLOG_UNIT_NAME xLinkWinUsb
#include "XLinkLog.h"
#include "XLinkStringUtils.h"

#define USB_DIR_OUT     0
#define USB_DIR_IN      1

#define USB_DEV_NONE    NULL
#define USB_HAN_NONE    NULL

#define USB_ERR_NONE        0
#define USB_ERR_TIMEOUT     -1
#define USB_ERR_FAILED      -2
#define USB_ERR_INVALID     -3



///*
struct ep_info {
    uint8_t ep;
    size_t sz;
    ULONG last_timeout;
};
struct _usb_han {
    HANDLE devHan;
    WINUSB_INTERFACE_HANDLE winUsbHan;
    struct ep_info eps[2];
};

extern const char * usb_get_pid_name(int);
extern int isMyriadDevice(const int idVendor, const int idProduct);
extern int isBootedMyriadDevice(const int idVendor, const int idProduct);
extern int isBootloaderMyriadDevice(const int idVendor, const int idProduct);
extern int isFlashBootedMyriadDevice(const int idVendor, const int idProduct);
extern int isNotBootedMyriadDevice(const int idVendor, const int idProduct);

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

// Myriad 2: {19E08104-0543-40A5-B107-87EF463DCEF1}
DEFINE_GUID(GUID_DEVINTERFACE_Myriad2, 0x19e08104, 0x0543, 0x40a5,
0xb1, 0x07, 0x87, 0xef, 0x46, 0x3d, 0xce, 0xf1);

// Myriad X: {504E1220-E189-413A-BDEC-ECFFAF3A3731}
DEFINE_GUID(GUID_DEVINTERFACE_MyriadX, 0x504e1220, 0xe189, 0x413a,
0xbd, 0xec, 0xec, 0xff, 0xaf, 0x3a, 0x37, 0x31);

static FILE *msgfile = NULL;
static int verbose = 0, ignore_errors = 0;
static DWORD last_bulk_errcode = 0;
static char *errmsg_buff = NULL;
static size_t errmsg_buff_len = 0;

static int MX_ID_TIMEOUT = 100; // 100ms


static const char* gen_addr_mx_id(HDEVINFO devInfo, SP_DEVINFO_DATA* devInfoData, int pid, char** refDevicePath);


static const char *format_win32_msg(DWORD errId) {
    while(!FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL, errId, 0, errmsg_buff, (DWORD)errmsg_buff_len, NULL)) {
        if(GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            err_fail:
            snprintf(errmsg_buff, errmsg_buff_len, "Win32 Error 0x%08lx (Unable to retrieve error message)", errId);
            return errmsg_buff;
        }
        size_t nlen = errmsg_buff_len + (errmsg_buff_len / 2);
        if(nlen > 1024)
            goto err_fail;
        char *nbuff = realloc(errmsg_buff, nlen);
        if(nbuff == NULL)
            goto err_fail;
        errmsg_buff = nbuff;
        errmsg_buff_len = nlen;
    }
    return errmsg_buff;
}

static void wperror(const char *errmsg) {
    DWORD errId = GetLastError();
    mvLog(MVLOG_ERROR, "%s: System err %d\n", errmsg, errId);
}

static void wstrerror(char *buff, const char *errmsg) {
    DWORD errId = GetLastError();
    snprintf(buff,strlen(buff), "%s: %s\n", errmsg, format_win32_msg(errId));
}
const char* libusb_strerror(int x)
{
    return format_win32_msg(x);
}
int usb_init(void) {
    msgfile = stdout;
    if(errmsg_buff == NULL) {
        errmsg_buff_len = 64;
        errmsg_buff = malloc(errmsg_buff_len);
        if(errmsg_buff == NULL) {
            perror("malloc");
            return -1;
        }
    }
    return 0;
}

void usb_shutdown(void) {
    if(errmsg_buff != NULL) {
        free(errmsg_buff);
        errmsg_buff = NULL;
    }
}

int usb_can_find_by_guid(void) {
    return 1;
}

static usb_dev retreive_dev_path(HDEVINFO devInfo, SP_DEVICE_INTERFACE_DATA *ifaceData) {
    usb_dev res;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detData;
    ULONG len, reqLen;

    if(!SetupDiGetDeviceInterfaceDetail(devInfo, ifaceData, NULL, 0, &reqLen, NULL)) {
        if(GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            wperror("SetupDiEnumDeviceInterfaces");
            SetupDiDestroyDeviceInfoList(devInfo);
            return USB_DEV_NONE;
        }
    }
    detData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)_alloca(reqLen);
    detData->cbSize = sizeof(*detData);
    len = reqLen;
    if(!SetupDiGetDeviceInterfaceDetail(devInfo, ifaceData, detData, len, &reqLen, NULL)) {
        wperror("SetupDiGetDeviceInterfaceDetail");
        SetupDiDestroyDeviceInfoList(devInfo);
        return USB_DEV_NONE;
    }
    res = _strdup(detData->DevicePath);
    if(res == NULL) {
        perror("strdup");
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return res;
}



static const char *gen_addr(HDEVINFO devInfo, SP_DEVINFO_DATA *devInfoData, int pid) {
    static char buff[16];
    char li_buff[128];
    unsigned int port, hub;
    if (!SetupDiGetDeviceRegistryProperty(devInfo, devInfoData, SPDRP_LOCATION_INFORMATION, NULL, li_buff, sizeof(li_buff), NULL))
    {
        goto ret_err;
    }
    if(sscanf(li_buff, "Port_#%u.Hub_#%u", &port, &hub) != 2)
        goto ret_err;

    //matching it to libusboutput
    const char* dev_name = usb_get_pid_name(pid);
    if(dev_name == NULL)
        goto ret_err;

    snprintf(buff, sizeof(buff), "%u.%u-%s", hub, port, dev_name);
    buff[sizeof(buff) - 1] = '\0';
    return buff;
    ret_err:
    return "<error>";
}



static int compareDeviceByHubAndPort(const void *l, const void *r) {
    int lHub = 0, lPort = 0;
    int rHub = 0, rPort = 0;

    if (sscanf(((const char *)l + 4), "%d.%d", &lHub, &lPort) == EOF) {
        perror("Can not parse hub and port of the devices");
    };
    if (sscanf(((const char *)r + 4), "%d.%d", &rHub, &rPort) == EOF) {
        perror("Can not parse hub and port of the devices");
    }

    if (lHub != rHub) {
        return rHub - lHub;
    }

    return rPort - lPort;
}


usb_dev findDeviceByGUID(GUID guid, int loud)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA ifaceData;

    devInfo = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        wperror("SetupDiGetClassDevs");
        return USB_DEV_NONE;
    }
    ifaceData.cbSize = sizeof(ifaceData);
    if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &guid, 0, &ifaceData)) {
        if (GetLastError() != ERROR_NO_MORE_ITEMS) {
            wperror("SetupDiEnumDeviceInterfaces");
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        return USB_DEV_NONE;
    }
    return retreive_dev_path(devInfo, &ifaceData);
}

void * usb_find_device_by_guid(int loud) {
    void *dev = USB_DEV_NONE;
    //Try Myriad 2
    dev = findDeviceByGUID(GUID_DEVINTERFACE_Myriad2, loud);
    if (dev == USB_DEV_NONE)
    {
        //Try Myriad X
        dev = findDeviceByGUID(GUID_DEVINTERFACE_MyriadX, loud);
    }
    return dev;
}

int usb_check_connected(usb_dev dev) {
    HANDLE han;
    if(dev == USB_DEV_NONE)
        return 0;
    han = CreateFile(dev, 0, FILE_SHARE_WRITE | FILE_SHARE_READ,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if(han == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(han);
    return 1;
}


UsbSpeed_t usb_get_usb_speed(usb_hwnd han){

    // TODO winusb api doesn't support getting other device speeds
    /*
    uint8_t devSpeed = 0;
    BOOL bResult = TRUE;
    ULONG length = sizeof(UCHAR);
    bResult = WinUsb_QueryDeviceInformation(han->winUsbHan, DEVICE_SPEED, &length, &devSpeed);
    if(!bResult) {
        printf("Error getting device speed: %d.\n", GetLastError());
        return X_LINK_USB_SPEED_UNKNOWN;
    }
    switch (devSpeed){
        case LowSpeed: return X_LINK_USB_SPEED_LOW;
        case FullSpeed: return X_LINK_USB_SPEED_FULL;
        case HighSpeed: return X_LINK_USB_SPEED_HIGH;
    }

    */

    // return UNKNOWN for now
    return X_LINK_USB_SPEED_UNKNOWN;

}




void * usb_open_device(usb_dev dev, uint8_t *ep, uint8_t intfaceno, char *err_string_buff, size_t err_max_len) {
    HANDLE devHan = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winUsbHan = INVALID_HANDLE_VALUE;
    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    WINUSB_PIPE_INFORMATION pipeInfo;
    usb_hwnd han = NULL;
    int i;

    if(dev == USB_DEV_NONE)
        return USB_HAN_NONE;

    devHan = CreateFile(dev, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if(devHan == INVALID_HANDLE_VALUE) {
        if(err_string_buff != NULL)
            wstrerror(err_string_buff, "CreateFile");
        goto exit_err;
    }

    if(!WinUsb_Initialize(devHan, &winUsbHan)) {
        if (err_string_buff != NULL)
            wstrerror(err_string_buff, "WinUsb_Initialize");
        goto exit_err;
    }

    if(!WinUsb_QueryInterfaceSettings(winUsbHan, 0, &ifaceDesc)) {
        if (err_string_buff != NULL)
            wstrerror(err_string_buff, "WinUsb_QueryInterfaceSettings");
        goto exit_err;
    }

    han = calloc(1, sizeof(*han));
    if(han == NULL) {
        mv_strcpy(err_string_buff, err_max_len, _strerror("malloc"));
        goto exit_err;
    }
    han->devHan = devHan;
    han->winUsbHan = winUsbHan;

    for(i=0; i<ifaceDesc.bNumEndpoints; i++) {
        if(!WinUsb_QueryPipe(winUsbHan, 0, i, &pipeInfo)) {
            if (err_string_buff != NULL)
                wstrerror(err_string_buff, "WinUsb_QueryPipe");
            if(!ignore_errors)
                goto exit_err;
        }
        if(verbose) {
            fprintf(msgfile, "Found EP 0x%02x : max packet size is %u bytes\n",
                    pipeInfo.PipeId, pipeInfo.MaximumPacketSize);
        }
        if(pipeInfo.PipeType != UsbdPipeTypeBulk)
            continue;
        int ind = USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId) ? USB_DIR_IN : USB_DIR_OUT;
        han->eps[ind].ep = pipeInfo.PipeId;
        han->eps[ind].sz = pipeInfo.MaximumPacketSize;
        han->eps[ind].last_timeout = 0;
    }
    if(ep)
        *ep = han->eps[USB_DIR_OUT].ep;

    if(err_string_buff && (han->eps[USB_DIR_IN].ep == 0)) {
        sprintf_s(err_string_buff, strnlen(err_string_buff, err_max_len), "Unable to find BULK IN endpoint\n");
        goto exit_err;
    }
    if(err_string_buff && (han->eps[USB_DIR_OUT].ep == 0)) {
        sprintf_s(err_string_buff, strnlen(err_string_buff, err_max_len), "Unable to find BULK OUT endpoint\n");
        goto exit_err;
    }
    if(err_string_buff && (han->eps[USB_DIR_IN].sz == 0)) {
        sprintf_s(err_string_buff, strnlen(err_string_buff, err_max_len), "Unable to find BULK IN endpoint size\n");
        goto exit_err;
    }
    if(err_string_buff && (han->eps[USB_DIR_OUT].sz == 0)) {
        sprintf_s(err_string_buff, strnlen(err_string_buff, err_max_len), "Unable to find BULK OUT endpoint size\n");
        goto exit_err;
    }
    return han;
    exit_err:

    if (winUsbHan != INVALID_HANDLE_VALUE) {
        WinUsb_Free(winUsbHan);
    }

    if (devHan != INVALID_HANDLE_VALUE) {
        CloseHandle(devHan);
    }

    if(han != NULL) {
        free(han);
    }

    return USB_HAN_NONE;
}

uint8_t usb_get_bulk_endpoint(usb_hwnd han, int dir) {
    if((han == NULL) || ((dir != USB_DIR_OUT) && (dir != USB_DIR_IN)))
        return 0;
    return han->eps[dir].ep;
}

size_t usb_get_endpoint_size(usb_hwnd han, uint8_t ep) {
    if(han == NULL)
        return 0;
    if(han->eps[USB_DIR_OUT].ep == ep)
        return han->eps[USB_DIR_OUT].sz;
    if(han->eps[USB_DIR_IN].ep == ep)
        return han->eps[USB_DIR_IN].sz;
    return 0;
}

int usb_control_transfer(usb_hwnd han, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, const void *buffer, size_t sz, uint32_t *wrote_bytes, int timeout_ms) {
    ULONG wb = 0;
    if(wrote_bytes != NULL)
        *wrote_bytes = 0;
    if(han == NULL)
        return USB_ERR_INVALID;

    // Timeout variables
    ULONG prevTimeout = 0;
    ULONG prevTimeoutSize = sizeof(prevTimeout);

    // Get previous timeout on control endpoint (0)
    if (!WinUsb_GetPipePolicy(han->winUsbHan, 0, PIPE_TRANSFER_TIMEOUT, &prevTimeoutSize, &prevTimeout)) {
        wperror("WinUsb_GetPipePolicy");
        return USB_ERR_FAILED;
    }

    // Set given timeout
    ULONG timeout = timeout_ms;
    if(!WinUsb_SetPipePolicy(han->winUsbHan, 0, PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeout)){
        wperror("WinUsb_SetPipePolicy");
        return USB_ERR_FAILED;
    }

    // Create setup packet
    WINUSB_SETUP_PACKET setup = {
        .RequestType = bmRequestType,
        .Request = bRequest,
        .Value = wValue,
        .Index = wIndex,
        .Length = sz
    };

    // Make control transfer
    if(!WinUsb_ControlTransfer(han->winUsbHan, setup, buffer, sz, wrote_bytes, NULL)){
        if(GetLastError() == ERROR_SEM_TIMEOUT){
            mvLog(MVLOG_ERROR, "WinUsb_ControlTransfer timeout\n");
            return USB_ERR_TIMEOUT;
        }
        wperror("WinUsb_ControlTransfer");
        mvLog(MVLOG_ERROR, "WinUsb_ControlTransfer failed with error:=%d\n", GetLastError());
        return USB_ERR_FAILED;
    }

    // Set back previous timeout
    if (!WinUsb_SetPipePolicy(han->winUsbHan, 0, PIPE_TRANSFER_TIMEOUT, sizeof(prevTimeout), &prevTimeout)) {
        wperror("WinUsb_SetPipePolicy");
        return USB_ERR_FAILED;
    }

    return USB_ERR_NONE;
}

int usb_bulk_write(usb_hwnd han, uint8_t ep, const void *buffer, size_t sz, uint32_t *wrote_bytes, int timeout_ms) {
    ULONG wb = 0;
    if(wrote_bytes != NULL)
        *wrote_bytes = 0;
    if(han == NULL)
        return USB_ERR_INVALID;

    if(timeout_ms != han->eps[USB_DIR_OUT].last_timeout) {
        han->eps[USB_DIR_OUT].last_timeout = timeout_ms;
        if(!WinUsb_SetPipePolicy(han->winUsbHan, ep, PIPE_TRANSFER_TIMEOUT,
                                 sizeof(ULONG), &han->eps[USB_DIR_OUT].last_timeout)) {
            last_bulk_errcode = GetLastError();
            wperror("WinUsb_SetPipePolicy");
            return USB_ERR_FAILED;
        }
    }
    if(!WinUsb_WritePipe(han->winUsbHan, ep, (PUCHAR)buffer, (ULONG)sz, &wb, NULL)) {
        last_bulk_errcode = GetLastError();
        if(last_bulk_errcode == ERROR_SEM_TIMEOUT)
            return USB_ERR_TIMEOUT;
        wperror("WinUsb_WritePipe");
        mvLog(MVLOG_ERROR, "\nWinUsb_WritePipe failed with error:=%d\n", GetLastError());
        return USB_ERR_FAILED;
    }
    last_bulk_errcode = 0;
    if(wrote_bytes != NULL)
        *wrote_bytes = wb;
    return USB_ERR_NONE;
}

int usb_bulk_read(usb_hwnd han, uint8_t ep, void *buffer, size_t sz, uint32_t *read_bytes, int timeout_ms) {
    ULONG rb = 0;
    if(read_bytes != NULL)
        *read_bytes = 0;
    if(han == NULL)
        return USB_ERR_INVALID;

    if(timeout_ms != han->eps[USB_DIR_IN].last_timeout) {
        han->eps[USB_DIR_IN].last_timeout = timeout_ms;
        if(!WinUsb_SetPipePolicy(han->winUsbHan, ep, PIPE_TRANSFER_TIMEOUT,
                                 sizeof(ULONG), &han->eps[USB_DIR_IN].last_timeout)) {
            last_bulk_errcode = GetLastError();
            wperror("WinUsb_SetPipePolicy");
            return USB_ERR_FAILED;
        }
    }
    if(sz == 0)
        return USB_ERR_NONE;
    if(!WinUsb_ReadPipe(han->winUsbHan, ep, buffer, (ULONG)sz, &rb, NULL)) {
        last_bulk_errcode = GetLastError();
        if(last_bulk_errcode == ERROR_SEM_TIMEOUT)
            return USB_ERR_TIMEOUT;
        return USB_ERR_FAILED;
    }
    last_bulk_errcode = 0;
    if(read_bytes != NULL)
        *read_bytes = rb;
    return USB_ERR_NONE;
}

void usb_free_device(usb_dev dev) {
    if(dev != NULL)
        free(dev);
}

void usb_close_device(usb_hwnd han) {
    if(han == NULL)
        return;
    WinUsb_Free(han->winUsbHan);
    CloseHandle(han->devHan);
    free(han);
}

const char *usb_last_bulk_errmsg(void) {
    return format_win32_msg(last_bulk_errcode);
}

void usb_set_msgfile(FILE *file) {
    msgfile = file;
}

void usb_set_verbose(int value) {
    verbose = value;
}

void usb_set_ignoreerrors(int value) {
    ignore_errors = value;
}




static const char* get_mx_id_device_path(HDEVINFO devInfo, SP_DEVINFO_DATA* devInfoData, char** devicePath) {

    static char mx_id[XLINK_MAX_MX_ID_SIZE] = {0};
    static char device_path[1024] = {0};

    DWORD requiredLength = 0;
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData = NULL;

    // Requires an interface GUID, for which I have none to specify
    deviceInterfaceData.cbSize = sizeof(SP_INTERFACE_DEVICE_DATA);
    if (!SetupDiEnumDeviceInterfaces(devInfo, devInfoData, &GUID_DEVINTERFACE_USB_DEVICE, 0, &deviceInterfaceData)) {
        return NULL;
    }

    if (!SetupDiGetDeviceInterfaceDetail(devInfo, &deviceInterfaceData, NULL, 0, &requiredLength, NULL)) {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && requiredLength > 0) {
            deviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)LocalAlloc(LPTR, requiredLength);


            deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (!SetupDiGetDeviceInterfaceDetail(devInfo, &deviceInterfaceData, deviceInterfaceDetailData, requiredLength, NULL, devInfoData)) {
                return NULL;
            }

            uint16_t det_vid, det_pid;

            // parse serial number (apperantly the way on Wins)
            sscanf(deviceInterfaceDetailData->DevicePath, "\\\\?\\usb#vid_%hx&pid_%hx#%[^#]", &det_vid, &det_pid, mx_id);
            mvLog(MVLOG_DEBUG, "mx id found: %s\n", mx_id);

            if (devicePath != NULL) {
                // This could be a composite device, in which case we must retrieve the XLink VSC interface path
                DEVINST devInstNext;
                if (CM_Get_Child(&devInstNext, devInfoData->DevInst, 0) == CR_SUCCESS) {
                    char id[256] = "";
                    CM_Get_Device_IDA(devInstNext, id, sizeof(id), 0);
                    // Getting the full path through the Win32 API is utterly complicated, TODO fix up later
                    for (int i = 0; id[i] != 0 && i < sizeof(id); i++) {
                        if (id[i] == '\\') id[i] = '#';
                    }
                    const char *winusb_guid = "8FE6D4D7-49DD-41E7-9486-49AFC6BFE475";
                    snprintf(device_path, sizeof(device_path), "\\\\?\\%s#{%s}", id, winusb_guid);
                }
                else {
                    snprintf(device_path, sizeof(device_path), "%s", deviceInterfaceDetailData->DevicePath);
                }
                *devicePath = &device_path[0];
            }

            return mx_id;


            if (!deviceInterfaceDetailData) {
                return NULL;
            }
        }
        else {
            return NULL;
        }
    }

    return NULL;

}



#include "win_time.h"
static double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}


static const char* gen_addr_mx_id(HDEVINFO devInfo, SP_DEVINFO_DATA* devInfoData, int pid, char** refDevicePath) {

    // initialize cache
    usb_mx_id_cache_init();

    // Static variables
    static char final_addr[XLINK_MAX_NAME_SIZE];
    static char mx_id[XLINK_MAX_MX_ID_SIZE];
    static char device_path[1024];

    // Set final_addr as error first
    strncpy(final_addr, "<error>", sizeof(final_addr));

    // generate unique (full) usb bus-port path
    const char* compat_addr = gen_addr(devInfo, devInfoData, pid);

    // first check if entry already exists in the list (and is still valid)
    // if found, it stores it into mx_id variable
    bool found = usb_mx_id_cache_get_entry(compat_addr, mx_id);


    // Get mx id for booted devices (and devicePath) - non intrusive operation
    char* devicePath = NULL;
    const char* booted_mx_id = get_mx_id_device_path(devInfo, devInfoData, &devicePath);
    if (devicePath == NULL) {
        return NULL;
    }
    // Create a local copy (which is valid until the next gen_addr_mx_id call
    strncpy(device_path, devicePath, sizeof(device_path));
    if (refDevicePath != NULL) {
        *refDevicePath = device_path;
    }


    if (found) {
        mvLog(MVLOG_DEBUG, "Found cached MX ID: %s", mx_id);
    } else {
        // If not found, retrieve mx_id

        // if UNBOOTED state, perform mx_id retrieval procedure using small program and a read command
        if (pid == DEFAULT_UNBOOTPID_2485 || pid == DEFAULT_UNBOOTPID_2150) {

            // get serial from usb descriptor
            libusb_device_handle* handle = NULL;
            int libusb_rc = 0;

            // Open device
            char last_open_dev_err[128] = { 0 };
            handle = usb_open_device(devicePath, NULL, 0, last_open_dev_err, sizeof(last_open_dev_err));
            if (handle == NULL) {
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

                const int send_ep = 0x01;
                const int size = usb_mx_id_get_payload_size();
                int transferred = 0;
                if ((libusb_rc = usb_bulk_write(handle, send_ep, usb_mx_id_get_payload(), size, &transferred, MX_ID_TIMEOUT)) < 0) {
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
                if ((libusb_rc = usb_bulk_read(handle, recv_ep, rbuf, sizeof(rbuf), &transferred, MX_ID_TIMEOUT)) < 0) {
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


                // Parse mx_id into HEX presentation
                // There's a bug, it should be 0x0F, but setting as in MDK
                rbuf[8] &= 0xF0;

                // Convert to HEX presentation and store into mx_id
                for (int i = 0; i < transferred; i++) {
                    sprintf(mx_id + 2 * i, "%02X", rbuf[i]);
                }

                // Indicate no error
                libusb_rc = 0;

            } while (libusb_rc != 0 && seconds() - t_retry < RETRY_TIMEOUT);

            // Close opened device
            usb_close_device(handle);

            // if mx_id couldn't be retrieved, exit by returning final_addr ("<error>")
            if (libusb_rc != 0) {
                return final_addr;
            }

        } else {

            // copy serial retrieved from booted device (device path OS cached)
            strncpy(mx_id, booted_mx_id, sizeof(mx_id));

        }

        // Cache the retrieved mx_id
        // Find empty space and store this entry
        // If no empty space, don't cache (possible case: >16 devices)
        int cache_index = usb_mx_id_cache_store_entry(mx_id, compat_addr);
        if (cache_index >= 0) {
            // debug print
            mvLog(MVLOG_DEBUG, "Cached MX ID %s at index %d", mx_id, cache_index);
        }
        else {
            // debug print
            mvLog(MVLOG_DEBUG, "Couldn't cache MX ID %s", mx_id);
        }

    }

    // At the end add dev_name to retain compatibility with rest of the codebase
    const char* dev_name = usb_get_pid_name(pid);

    // convert mx_id to uppercase
    for (int i = 0; i < XLINK_MAX_MX_ID_SIZE; i++) {
        if (mx_id[i] == 0) break;

        if (mx_id[i] >= 'a' && mx_id[i] <= 'z') {
            mx_id[i] = mx_id[i] - 32;
        }
    }

    // Create address [mx_id]-[dev_name]
    snprintf(final_addr, sizeof(final_addr), "%s-%s", mx_id, dev_name);

    mvLog(MVLOG_DEBUG, "Returning generated name: %s (booted mx id: %s, compat addr: %s)", final_addr, booted_mx_id, compat_addr);

    return final_addr;

}


typedef struct {
    uint16_t vid;
    uint16_t pid;
} vid_pid_pair;


typedef struct {
    HDEVINFO devInfo;
    SP_DEVINFO_DATA* infos;
    vid_pid_pair* vidpids;
} usb_dev_list;

void usb_list_free_devices(usb_dev_list* list) {

    // Free dev info
    SetupDiDestroyDeviceInfoList(list->devInfo);

    // free infos and vidpids
    free(list->infos);
    free(list->vidpids);

    // free structure itself
    free(list);

}


int usb_get_device_list(usb_dev_list** refPDevList) {

    // Create list
    if (refPDevList == NULL) return -1;

    *refPDevList = calloc(1, sizeof(usb_dev_list));
    usb_dev_list* pDevList = *refPDevList;
    const int MAX_NUM_DEVICES = 128;

    int i;
    char hwid_buff[128];

    pDevList->devInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (pDevList->devInfo == INVALID_HANDLE_VALUE) {
        wperror("SetupDiGetClassDevs");
        return -1;
    }

    // create list
    pDevList->infos = calloc(MAX_NUM_DEVICES, sizeof(SP_DEVINFO_DATA));
    pDevList->vidpids = calloc(MAX_NUM_DEVICES, sizeof(vid_pid_pair));

    for (i = 0; i < MAX_NUM_DEVICES; i++) {
        pDevList->infos[i].cbSize = sizeof(SP_DEVINFO_DATA);
    }

    //    devInfoData.cbSize = sizeof(devInfoData);
    for (i = 0; SetupDiEnumDeviceInfo(pDevList->devInfo, i, pDevList->infos + i) && i < MAX_NUM_DEVICES; i++) {
        if (!SetupDiGetDeviceRegistryProperty(pDevList->devInfo, pDevList->infos + i, SPDRP_HARDWAREID, NULL, hwid_buff, sizeof(hwid_buff), NULL)) {
            continue;
        }
        uint16_t fvid, fpid;
        if (sscanf(hwid_buff, "USB\\VID_%hx&PID_%hx", (int16_t*)&fvid, (int16_t*)&fpid) != 2) {
            continue;
        }

        pDevList->vidpids[i].vid = fvid;
        pDevList->vidpids[i].pid = fpid;

    }

    return i;
}


#if (defined(_WIN32) || defined(_WIN64) )
int win_usb_find_device(unsigned idx, char* addr, unsigned addrsize, void** device, int vid, int pid)
{
    if (!addr)
        return USB_BOOT_ERROR;
    int specificDevice = 0;
    if (strlen(addr) > 1)
        specificDevice = 1;

    // TODO There is no global mutex as in linux version
    int res;

    static usb_dev_list* devs = NULL;
    static int devs_cnt = 0;
    int count = 0;

    // Update device list if empty or if indx 0
    if (devs == NULL || idx == 0) {
        if (devs) {
            usb_list_free_devices(devs);
            devs = 0;
        }
        if ((res = usb_get_device_list(&devs)) < 0) {
            mvLog(MVLOG_DEBUG, "Unable to get USB device list: %s", libusb_strerror(res));
            return USB_BOOT_ERROR;
        }
        devs_cnt = res;
    }

    for (int i = 0; i < devs_cnt; i++) {

        // retrieve vid & pid
        int idVendor = (int) devs->vidpids[i].vid;
        int idProduct = (int) devs->vidpids[i].pid;

        // If found device have the same id and vid as input
        if ((idVendor == vid && idProduct == pid)
            // Any myriad device
            || (vid == AUTO_VID && pid == AUTO_PID
                && isMyriadDevice(idVendor, idProduct))
            // Any unbooted myriad device
            || (vid == AUTO_VID && (pid == AUTO_UNBOOTED_PID)
                && isNotBootedMyriadDevice(idVendor, idProduct))
            // Any unbooted with same pid
            || (vid == AUTO_VID && pid == idProduct
                && isNotBootedMyriadDevice(idVendor, idProduct))
            // Any booted device
            || (vid == AUTO_VID && pid == DEFAULT_OPENPID
                && isBootedMyriadDevice(idVendor, idProduct))
            // Any bootloader device
            || (vid == AUTO_VID && pid == DEFAULT_BOOTLOADER_PID
                && isBootloaderMyriadDevice(idVendor, idProduct))
            // Any flash booted device
            || (vid == AUTO_VID && pid == DEFAULT_FLASH_BOOTED_PID
                && isFlashBootedMyriadDevice(idVendor, idProduct))
            ) {
            if (device) {

                // device path to be retrieved from gen_addr_* call
                char* devicePath = NULL;
                // gen addr
                const char* caddr = gen_addr_mx_id(devs->devInfo, devs->infos + i, idProduct, &devicePath);
                if (strncmp(addr, caddr, XLINK_MAX_NAME_SIZE) == 0)
                {
                    mvLog(MVLOG_DEBUG, "Found Address: %s - VID/PID %04x:%04x", caddr, idVendor, idProduct);

                    // Create a copy of device path string. It will be freed
                    *device = strdup(devicePath);
                    devs_cnt = 0;
                    return USB_BOOT_SUCCESS;
                }
            }
            else if (specificDevice) {

                // gen addr
                const char* caddr = gen_addr_mx_id(devs->devInfo, devs->infos + i, idProduct, NULL);

                if (strncmp(addr, caddr, XLINK_MAX_NAME_SIZE) == 0)
                {
                    mvLog(MVLOG_DEBUG, "Found Address: %s - VID/PID %04x:%04x", caddr, idVendor, idProduct, NULL);
                    return USB_BOOT_SUCCESS;
                }
            }
            else if (idx == count)
            {
                // gen addr
                const char* caddr = gen_addr_mx_id(devs->devInfo, devs->infos + i, idProduct, NULL);

                mvLog(MVLOG_DEBUG, "Device %d Address: %s - VID/PID %04x:%04x", idx, caddr, idVendor, idProduct);
                mv_strncpy(addr, addrsize, caddr, addrsize - 1);
                return USB_BOOT_SUCCESS;
            }
            count++;
        }
    }
    devs_cnt = 0;
    return USB_BOOT_DEVICE_NOT_FOUND;
}
#endif

