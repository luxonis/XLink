#pragma once

#include <cstddef>

struct libusb_device_handle;

namespace usb_test {
libusb_device_handle* openHandle();
bool transferFits(unsigned char* data, std::size_t size);
void checkClose(libusb_device_handle* handle);
void checkReceiveMemory();
}
