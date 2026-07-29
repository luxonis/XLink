fn main() {
    let devices = xlink::find_devices(&xlink::DeviceQuery::default()).expect("device search");
    if devices.is_empty() {
        println!("No devices found.");
        return;
    }
    for device in devices {
        println!(
            "name: {}, mxid: {}, state: {}, protocol: {:?}, platform: {:?}",
            device.name, device.mxid, device.state, device.protocol, device.platform
        );
    }
}
