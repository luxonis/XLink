# XLink Rust bindings

Rust bindings to XLink, split into two crates:

- `xlink-sys` — raw FFI bindings. Compiles the XLink C/C++ sources from this
  repository directly via the [`cc`](https://crates.io/crates/cc) crate (no
  CMake or Hunter required). By default the USB (libusb) protocol is disabled,
  so only TCP/IP discovery is available and there are no external native
  dependencies. Enable the `libusb` feature to compile the USB protocol
  (requires libusb-1.0, discovered via pkg-config).
- `xlink` — safe, idiomatic wrapper. Currently covers library initialization
  and device discovery (`find_devices`), which is enough to enumerate RVC2
  devices over the network (and over USB with the `libusb` feature).

## Usage

```toml
[dependencies]
xlink = { git = "https://github.com/luxonis/XLink" }
```

```rust
let devices = xlink::find_tcpip_devices()?;
for device in devices {
    println!("{} {} ({})", device.mxid, device.name, device.state);
}
```

## Example

```sh
cargo run --example list_devices
```
