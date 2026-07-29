//! Raw FFI bindings to the XLink library.
//!
//! The declarations mirror `include/XLink/XLink.h` and
//! `include/XLink/XLinkPublicDefines.h`. Enum values are represented as plain
//! integers (`c_int` newtypes via type aliases + constants) so that unknown
//! values coming from the C side never cause undefined behaviour.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use std::os::raw::{c_char, c_int, c_ulong, c_void};

pub const XLINK_MAX_MX_ID_SIZE: usize = 32;
pub const XLINK_MAX_NAME_SIZE: usize = 64;

// XLinkError_t
pub type XLinkError_t = c_int;
pub const X_LINK_SUCCESS: XLinkError_t = 0;
pub const X_LINK_ALREADY_OPEN: XLinkError_t = 1;
pub const X_LINK_COMMUNICATION_NOT_OPEN: XLinkError_t = 2;
pub const X_LINK_COMMUNICATION_FAIL: XLinkError_t = 3;
pub const X_LINK_COMMUNICATION_UNKNOWN_ERROR: XLinkError_t = 4;
pub const X_LINK_DEVICE_NOT_FOUND: XLinkError_t = 5;
pub const X_LINK_TIMEOUT: XLinkError_t = 6;
pub const X_LINK_ERROR: XLinkError_t = 7;
pub const X_LINK_OUT_OF_MEMORY: XLinkError_t = 8;
pub const X_LINK_INSUFFICIENT_PERMISSIONS: XLinkError_t = 9;
pub const X_LINK_DEVICE_ALREADY_IN_USE: XLinkError_t = 10;
pub const X_LINK_NOT_IMPLEMENTED: XLinkError_t = 11;
pub const X_LINK_INIT_USB_ERROR: XLinkError_t = 12;
pub const X_LINK_INIT_TCP_IP_ERROR: XLinkError_t = 13;
pub const X_LINK_INIT_PCIE_ERROR: XLinkError_t = 14;

// XLinkProtocol_t
pub type XLinkProtocol_t = c_int;
pub const X_LINK_USB_VSC: XLinkProtocol_t = 0;
pub const X_LINK_USB_CDC: XLinkProtocol_t = 1;
pub const X_LINK_PCIE: XLinkProtocol_t = 2;
pub const X_LINK_IPC: XLinkProtocol_t = 3;
pub const X_LINK_TCP_IP: XLinkProtocol_t = 4;
pub const X_LINK_NMB_OF_PROTOCOLS: XLinkProtocol_t = 5;
pub const X_LINK_ANY_PROTOCOL: XLinkProtocol_t = 6;

// XLinkPlatform_t
pub type XLinkPlatform_t = c_int;
pub const X_LINK_ANY_PLATFORM: XLinkPlatform_t = 0;
pub const X_LINK_MYRIAD_2: XLinkPlatform_t = 2450;
pub const X_LINK_MYRIAD_X: XLinkPlatform_t = 2480;

// XLinkDeviceState_t
pub type XLinkDeviceState_t = c_int;
pub const X_LINK_ANY_STATE: XLinkDeviceState_t = 0;
pub const X_LINK_BOOTED: XLinkDeviceState_t = 1;
pub const X_LINK_UNBOOTED: XLinkDeviceState_t = 2;
pub const X_LINK_BOOTLOADER: XLinkDeviceState_t = 3;
pub const X_LINK_FLASH_BOOTED: XLinkDeviceState_t = 4;
pub const X_LINK_BOOTED_NON_EXCLUSIVE: XLinkDeviceState_t = X_LINK_FLASH_BOOTED;

// mvLog_t
pub type mvLog_t = c_int;
pub const MVLOG_DEBUG: mvLog_t = 0;
pub const MVLOG_INFO: mvLog_t = 1;
pub const MVLOG_WARN: mvLog_t = 2;
pub const MVLOG_ERROR: mvLog_t = 3;
pub const MVLOG_FATAL: mvLog_t = 4;
pub const MVLOG_LAST: mvLog_t = 5;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct deviceDesc_t {
    pub protocol: XLinkProtocol_t,
    pub platform: XLinkPlatform_t,
    pub name: [c_char; XLINK_MAX_NAME_SIZE],
    pub state: XLinkDeviceState_t,
    pub mxid: [c_char; XLINK_MAX_MX_ID_SIZE],
    pub status: XLinkError_t,
    pub nameHintOnly: bool,
}

impl Default for deviceDesc_t {
    fn default() -> Self {
        // Equivalent of `deviceDesc_t desc = {};` in C
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct XLinkProf_t {
    pub totalReadTime: f32,
    pub totalWriteTime: f32,
    pub totalReadBytes: u64,
    pub totalWriteBytes: u64,
    pub totalBootCount: c_ulong,
    pub totalBootTime: f32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct XLinkGlobalHandler_t {
    pub profEnable: c_int,
    pub profilingData: XLinkProf_t,
    pub options: *mut c_void,
    // Deprecated fields
    pub loglevel: c_int,
    pub protocol: c_int,
}

impl Default for XLinkGlobalHandler_t {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

unsafe extern "C" {
    /// Initializes XLink and the scheduler. Safe to call multiple times.
    /// The passed handler must outlive all XLink usage (it is stored globally).
    pub fn XLinkInitialize(globalHandler: *mut XLinkGlobalHandler_t) -> XLinkError_t;

    pub fn XLinkIsProtocolInitialized(protocol: XLinkProtocol_t) -> c_int;

    pub fn XLinkFindFirstSuitableDevice(
        in_deviceRequirements: deviceDesc_t,
        out_foundDevice: *mut deviceDesc_t,
    ) -> XLinkError_t;

    pub fn XLinkFindAllSuitableDevices(
        in_deviceRequirements: deviceDesc_t,
        out_foundDevicesPtr: *mut deviceDesc_t,
        devicesArraySize: u32,
        out_foundDevicesCount: *mut u32,
    ) -> XLinkError_t;

    pub fn XLinkSearchForDevices(
        in_deviceRequirements: deviceDesc_t,
        out_foundDevicesPtr: *mut deviceDesc_t,
        devicesArraySize: u32,
        out_foundDevicesCount: *mut u32,
        timeoutMs: c_int,
        cb: Option<unsafe extern "C" fn(*mut deviceDesc_t, u32) -> bool>,
    ) -> XLinkError_t;

    pub fn XLinkBootBootloader(deviceDesc: *const deviceDesc_t) -> XLinkError_t;

    pub fn XLinkErrorToStr(val: XLinkError_t) -> *const c_char;
    pub fn XLinkProtocolToStr(val: XLinkProtocol_t) -> *const c_char;
    pub fn XLinkPlatformToStr(val: XLinkPlatform_t) -> *const c_char;
    pub fn XLinkDeviceStateToStr(val: XLinkDeviceState_t) -> *const c_char;

    /// Global default log level of the library (`MVLOGLEVEL(default)`).
    pub static mut mvLogLevel_default: mvLog_t;
    /// Global log level override (`MVLOGLEVEL(global)`).
    pub static mut mvLogLevel_global: mvLog_t;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_desc_layout() {
        // protocol(4) + platform(4) + name(64) + state(4) + mxid(32) +
        // status(4) + nameHintOnly(1) + padding(3)
        assert_eq!(std::mem::size_of::<deviceDesc_t>(), 116);
        assert_eq!(std::mem::align_of::<deviceDesc_t>(), 4);
    }

    #[test]
    fn error_to_str_roundtrip() {
        let s = unsafe { std::ffi::CStr::from_ptr(XLinkErrorToStr(X_LINK_DEVICE_NOT_FOUND)) };
        assert_eq!(s.to_str().unwrap(), "X_LINK_DEVICE_NOT_FOUND");
    }
}
