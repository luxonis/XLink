//! Safe Rust bindings to the XLink library.
//!
//! Currently focused on device discovery (`find_devices`), which is what is
//! needed to enumerate RVC2 devices (USB and PoE/TCP-IP).
//!
//! ```no_run
//! let devices = xlink::find_devices(&xlink::DeviceQuery::default()).unwrap();
//! for device in devices {
//!     println!("{} {} ({:?})", device.mxid, device.name, device.state);
//! }
//! ```

use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::OnceLock;

/// Errors returned by the XLink library, mirroring `XLinkError_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum Error {
    #[error("X_LINK_ALREADY_OPEN")]
    AlreadyOpen,
    #[error("X_LINK_COMMUNICATION_NOT_OPEN")]
    CommunicationNotOpen,
    #[error("X_LINK_COMMUNICATION_FAIL")]
    CommunicationFail,
    #[error("X_LINK_COMMUNICATION_UNKNOWN_ERROR")]
    CommunicationUnknownError,
    #[error("X_LINK_DEVICE_NOT_FOUND")]
    DeviceNotFound,
    #[error("X_LINK_TIMEOUT")]
    Timeout,
    #[error("X_LINK_ERROR")]
    Generic,
    #[error("X_LINK_OUT_OF_MEMORY")]
    OutOfMemory,
    #[error("X_LINK_INSUFFICIENT_PERMISSIONS")]
    InsufficientPermissions,
    #[error("X_LINK_DEVICE_ALREADY_IN_USE")]
    DeviceAlreadyInUse,
    #[error("X_LINK_NOT_IMPLEMENTED")]
    NotImplemented,
    #[error("X_LINK_INIT_USB_ERROR")]
    InitUsbError,
    #[error("X_LINK_INIT_TCP_IP_ERROR")]
    InitTcpIpError,
    #[error("X_LINK_INIT_PCIE_ERROR")]
    InitPcieError,
    #[error("unknown XLink error code {0}")]
    Unknown(i32),
}

fn check(code: xlink_sys::XLinkError_t) -> Result<(), Error> {
    match code {
        xlink_sys::X_LINK_SUCCESS => Ok(()),
        xlink_sys::X_LINK_ALREADY_OPEN => Err(Error::AlreadyOpen),
        xlink_sys::X_LINK_COMMUNICATION_NOT_OPEN => Err(Error::CommunicationNotOpen),
        xlink_sys::X_LINK_COMMUNICATION_FAIL => Err(Error::CommunicationFail),
        xlink_sys::X_LINK_COMMUNICATION_UNKNOWN_ERROR => Err(Error::CommunicationUnknownError),
        xlink_sys::X_LINK_DEVICE_NOT_FOUND => Err(Error::DeviceNotFound),
        xlink_sys::X_LINK_TIMEOUT => Err(Error::Timeout),
        xlink_sys::X_LINK_ERROR => Err(Error::Generic),
        xlink_sys::X_LINK_OUT_OF_MEMORY => Err(Error::OutOfMemory),
        xlink_sys::X_LINK_INSUFFICIENT_PERMISSIONS => Err(Error::InsufficientPermissions),
        xlink_sys::X_LINK_DEVICE_ALREADY_IN_USE => Err(Error::DeviceAlreadyInUse),
        xlink_sys::X_LINK_NOT_IMPLEMENTED => Err(Error::NotImplemented),
        xlink_sys::X_LINK_INIT_USB_ERROR => Err(Error::InitUsbError),
        xlink_sys::X_LINK_INIT_TCP_IP_ERROR => Err(Error::InitTcpIpError),
        xlink_sys::X_LINK_INIT_PCIE_ERROR => Err(Error::InitPcieError),
        other => Err(Error::Unknown(other)),
    }
}

/// Transport protocol of a device, mirroring `XLinkProtocol_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Protocol {
    UsbVsc,
    UsbCdc,
    Pcie,
    Ipc,
    TcpIp,
    Unknown(i32),
}

impl Protocol {
    fn from_raw(raw: xlink_sys::XLinkProtocol_t) -> Self {
        match raw {
            xlink_sys::X_LINK_USB_VSC => Self::UsbVsc,
            xlink_sys::X_LINK_USB_CDC => Self::UsbCdc,
            xlink_sys::X_LINK_PCIE => Self::Pcie,
            xlink_sys::X_LINK_IPC => Self::Ipc,
            xlink_sys::X_LINK_TCP_IP => Self::TcpIp,
            other => Self::Unknown(other),
        }
    }

    fn to_raw(self) -> xlink_sys::XLinkProtocol_t {
        match self {
            Self::UsbVsc => xlink_sys::X_LINK_USB_VSC,
            Self::UsbCdc => xlink_sys::X_LINK_USB_CDC,
            Self::Pcie => xlink_sys::X_LINK_PCIE,
            Self::Ipc => xlink_sys::X_LINK_IPC,
            Self::TcpIp => xlink_sys::X_LINK_TCP_IP,
            Self::Unknown(other) => other,
        }
    }
}

/// Device platform, mirroring `XLinkPlatform_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Platform {
    Myriad2,
    MyriadX,
    Unknown(i32),
}

impl Platform {
    fn from_raw(raw: xlink_sys::XLinkPlatform_t) -> Self {
        match raw {
            xlink_sys::X_LINK_MYRIAD_2 => Self::Myriad2,
            xlink_sys::X_LINK_MYRIAD_X => Self::MyriadX,
            other => Self::Unknown(other),
        }
    }

    fn to_raw(self) -> xlink_sys::XLinkPlatform_t {
        match self {
            Self::Myriad2 => xlink_sys::X_LINK_MYRIAD_2,
            Self::MyriadX => xlink_sys::X_LINK_MYRIAD_X,
            Self::Unknown(other) => other,
        }
    }
}

/// Device state, mirroring `XLinkDeviceState_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DeviceState {
    Booted,
    Unbooted,
    Bootloader,
    FlashBooted,
    Unknown(i32),
}

impl DeviceState {
    fn from_raw(raw: xlink_sys::XLinkDeviceState_t) -> Self {
        match raw {
            xlink_sys::X_LINK_BOOTED => Self::Booted,
            xlink_sys::X_LINK_UNBOOTED => Self::Unbooted,
            xlink_sys::X_LINK_BOOTLOADER => Self::Bootloader,
            xlink_sys::X_LINK_FLASH_BOOTED => Self::FlashBooted,
            other => Self::Unknown(other),
        }
    }

    fn to_raw(self) -> xlink_sys::XLinkDeviceState_t {
        match self {
            Self::Booted => xlink_sys::X_LINK_BOOTED,
            Self::Unbooted => xlink_sys::X_LINK_UNBOOTED,
            Self::Bootloader => xlink_sys::X_LINK_BOOTLOADER,
            Self::FlashBooted => xlink_sys::X_LINK_FLASH_BOOTED,
            Self::Unknown(other) => other,
        }
    }
}

impl std::fmt::Display for DeviceState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Booted => write!(f, "booted"),
            Self::Unbooted => write!(f, "unbooted"),
            Self::Bootloader => write!(f, "bootloader"),
            Self::FlashBooted => write!(f, "flash-booted"),
            Self::Unknown(other) => write!(f, "unknown({other})"),
        }
    }
}

/// A discovered device.
#[derive(Debug, Clone)]
pub struct DeviceInfo {
    /// Device path: an IP address for TCP/IP devices, a USB path (e.g.
    /// `1.4`) for USB devices.
    pub name: String,
    /// Device serial (MX ID), e.g. `14442C10D13EABCE00`.
    pub mxid: String,
    pub protocol: Protocol,
    pub platform: Platform,
    pub state: DeviceState,
}

/// Search filter for [`find_devices`]. The default value matches any device.
#[derive(Debug, Clone, Default)]
pub struct DeviceQuery {
    /// Only devices reachable over this protocol.
    pub protocol: Option<Protocol>,
    /// Only devices of this platform.
    pub platform: Option<Platform>,
    /// Only devices in this state.
    pub state: Option<DeviceState>,
    /// Exact device name (IP address / USB path) to look for.
    pub name: Option<String>,
    /// Exact device MX ID to look for.
    pub mxid: Option<String>,
}

/// Log verbosity of the underlying XLink library.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogLevel {
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
}

/// Sets the global log level of the underlying XLink library.
/// XLink logs to stdout/stderr directly; the default level is `Error`.
pub fn set_log_level(level: LogLevel) {
    let raw = match level {
        LogLevel::Debug => xlink_sys::MVLOG_DEBUG,
        LogLevel::Info => xlink_sys::MVLOG_INFO,
        LogLevel::Warn => xlink_sys::MVLOG_WARN,
        LogLevel::Error => xlink_sys::MVLOG_ERROR,
        LogLevel::Fatal => xlink_sys::MVLOG_FATAL,
        LogLevel::Off => xlink_sys::MVLOG_LAST,
    };
    unsafe {
        xlink_sys::mvLogLevel_default = raw;
    }
}

/// Initializes the XLink library (once per process). Called implicitly by
/// [`find_devices`].
///
/// Succeeds even when a protocol (e.g. USB without the `libusb` feature) is
/// unavailable; such protocols are simply skipped during discovery.
pub fn ensure_initialized() -> Result<(), Error> {
    static INIT: OnceLock<xlink_sys::XLinkError_t> = OnceLock::new();
    let status = *INIT.get_or_init(|| {
        // XLink stores the pointer globally, so the handler must be 'static.
        let handler = Box::leak(Box::new(xlink_sys::XLinkGlobalHandler_t::default()));
        unsafe { xlink_sys::XLinkInitialize(handler) }
    });
    check(status)
}

fn copy_to_c_array(dst: &mut [c_char], src: &str) {
    // leave space for the NUL terminator
    let capacity = dst.len() - 1;
    for (dst_byte, src_byte) in dst.iter_mut().take(capacity).zip(src.as_bytes()) {
        *dst_byte = *src_byte as c_char;
    }
}

fn c_array_to_string(src: &[c_char]) -> String {
    let bytes: Vec<u8> = src
        .iter()
        .take_while(|&&byte| byte != 0)
        .map(|&byte| byte as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

impl DeviceQuery {
    fn to_raw(&self) -> xlink_sys::deviceDesc_t {
        let mut desc = xlink_sys::deviceDesc_t {
            protocol: self
                .protocol
                .map_or(xlink_sys::X_LINK_ANY_PROTOCOL, Protocol::to_raw),
            platform: self
                .platform
                .map_or(xlink_sys::X_LINK_ANY_PLATFORM, Platform::to_raw),
            state: self
                .state
                .map_or(xlink_sys::X_LINK_ANY_STATE, DeviceState::to_raw),
            ..Default::default()
        };
        if let Some(name) = &self.name {
            copy_to_c_array(&mut desc.name, name);
        }
        if let Some(mxid) = &self.mxid {
            copy_to_c_array(&mut desc.mxid, mxid);
        }
        desc
    }
}

/// Maximum number of devices returned by a single [`find_devices`] call.
pub const MAX_DEVICES: usize = 64;

/// Finds all devices matching the query.
///
/// This performs blocking I/O (UDP broadcast for TCP/IP devices with a ~half
/// second timeout, USB enumeration when built with the `libusb` feature). Call
/// it from a blocking-friendly context (e.g. `tokio::task::spawn_blocking`).
pub fn find_devices(query: &DeviceQuery) -> Result<Vec<DeviceInfo>, Error> {
    ensure_initialized()?;

    let requirements = query.to_raw();
    let mut found = [xlink_sys::deviceDesc_t::default(); MAX_DEVICES];
    let mut count: u32 = 0;
    let result = check(unsafe {
        xlink_sys::XLinkFindAllSuitableDevices(
            requirements,
            found.as_mut_ptr(),
            found.len() as u32,
            &mut count,
        )
    });
    match result {
        // Reported when a protocol-specific search matches nothing.
        Err(Error::DeviceNotFound) => return Ok(Vec::new()),
        other => other?,
    }

    Ok(found[..count as usize]
        .iter()
        .map(|desc| DeviceInfo {
            name: c_array_to_string(&desc.name),
            mxid: c_array_to_string(&desc.mxid),
            protocol: Protocol::from_raw(desc.protocol),
            platform: Platform::from_raw(desc.platform),
            state: DeviceState::from_raw(desc.state),
        })
        .collect())
}

/// Convenience helper: finds all devices reachable over TCP/IP (e.g. RVC2 PoE
/// devices), regardless of state.
pub fn find_tcpip_devices() -> Result<Vec<DeviceInfo>, Error> {
    find_devices(&DeviceQuery {
        protocol: Some(Protocol::TcpIp),
        ..Default::default()
    })
}

/// Returns the string representation of an error as reported by XLink itself.
pub fn error_to_str(error: Error) -> &'static str {
    let raw = match error {
        Error::AlreadyOpen => xlink_sys::X_LINK_ALREADY_OPEN,
        Error::CommunicationNotOpen => xlink_sys::X_LINK_COMMUNICATION_NOT_OPEN,
        Error::CommunicationFail => xlink_sys::X_LINK_COMMUNICATION_FAIL,
        Error::CommunicationUnknownError => xlink_sys::X_LINK_COMMUNICATION_UNKNOWN_ERROR,
        Error::DeviceNotFound => xlink_sys::X_LINK_DEVICE_NOT_FOUND,
        Error::Timeout => xlink_sys::X_LINK_TIMEOUT,
        Error::Generic => xlink_sys::X_LINK_ERROR,
        Error::OutOfMemory => xlink_sys::X_LINK_OUT_OF_MEMORY,
        Error::InsufficientPermissions => xlink_sys::X_LINK_INSUFFICIENT_PERMISSIONS,
        Error::DeviceAlreadyInUse => xlink_sys::X_LINK_DEVICE_ALREADY_IN_USE,
        Error::NotImplemented => xlink_sys::X_LINK_NOT_IMPLEMENTED,
        Error::InitUsbError => xlink_sys::X_LINK_INIT_USB_ERROR,
        Error::InitTcpIpError => xlink_sys::X_LINK_INIT_TCP_IP_ERROR,
        Error::InitPcieError => xlink_sys::X_LINK_INIT_PCIE_ERROR,
        Error::Unknown(_) => xlink_sys::X_LINK_ERROR,
    };
    unsafe { CStr::from_ptr(xlink_sys::XLinkErrorToStr(raw)) }
        .to_str()
        .unwrap_or("X_LINK_ERROR")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init_succeeds_without_devices() {
        ensure_initialized().unwrap();
    }

    #[test]
    fn tcpip_discovery_runs() {
        // Should not error even when no devices are present on the network.
        let devices = find_tcpip_devices().unwrap();
        for device in devices {
            assert_eq!(device.protocol, Protocol::TcpIp);
        }
    }

    #[test]
    fn query_roundtrip() {
        let query = DeviceQuery {
            protocol: Some(Protocol::TcpIp),
            platform: Some(Platform::MyriadX),
            state: Some(DeviceState::Bootloader),
            name: Some("192.168.1.44".to_string()),
            mxid: Some("14442C10D13EABCE00".to_string()),
        };
        let raw = query.to_raw();
        assert_eq!(raw.protocol, xlink_sys::X_LINK_TCP_IP);
        assert_eq!(raw.platform, xlink_sys::X_LINK_MYRIAD_X);
        assert_eq!(raw.state, xlink_sys::X_LINK_BOOTLOADER);
        assert_eq!(c_array_to_string(&raw.name), "192.168.1.44");
        assert_eq!(c_array_to_string(&raw.mxid), "14442C10D13EABCE00");
    }
}
