// Win32 api and MSVC only; not mingw, *nix, etc.
#if defined(_WIN32) && defined(_MSC_VER)

// project
#define MVLOG_UNIT_NAME xLinkUsb

// libraries
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include "XLink/XLinkLog.h"
#include "usb_host.h"

// std
#include <array>
#include <pathcch.h>

int usbInitialize_customdir(void** hContext) {
    // get handle to the module containing a XLink static function/var
    // can not use GetModuleFileNameW(nullptr) because when the main depthai app is a DLL (e.g. plugin),
    // then it returns the main EXE which is usually wrong
    HMODULE hXLink = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&MVLOGLEVEL(global), &hXLink) == 0) {
        return LIBUSB_ERROR_OVERFLOW;
    }

    // get path to that module
    std::array<wchar_t, 2048> rawPath = {};
    const auto len = GetModuleFileNameW(hXLink, rawPath.data(), static_cast<DWORD>(rawPath.size()));
    if ((len == 0) || (len == rawPath.size())) {
        return LIBUSB_ERROR_OVERFLOW;
    }

    // remove filename with string::find_last_of(), _wsplitpath_s(), PathCchRemoveFileSpec()
    // using PathCchRemoveFileSpec() as it handles special cases
    if (PathCchRemoveFileSpec(rawPath.data(), rawPath.size()) != S_OK) {
        return LIBUSB_ERROR_OVERFLOW;
    }

    // persist existing custom DLL load path
    bool oldGetError = false;
    std::array<wchar_t, 2048> oldDllDir = {};
    if (GetDllDirectoryW(static_cast<DWORD>(oldDllDir.size()), oldDllDir.data())) {
        // nothing
    }
    else {
        // SetDllDirectoryW() previously unused, or an error
        oldGetError = true;
    }

    // set custom dll load directory
    if (SetDllDirectoryW(rawPath.data()) == 0) {
        return LIBUSB_ERROR_OVERFLOW;
    }

    // initialize libusb
    int initResult = LIBUSB_SUCCESS;
    __try {
        initResult = libusb_init((libusb_context**)hContext);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        initResult = LIBUSB_ERROR_OVERFLOW;
    }

    // restore custom dll load directory
    SetDllDirectoryW(oldGetError ? nullptr : oldDllDir.data());

    return initResult;
}

#endif // defined(_WIN32) && defined(_MSC_VER)
