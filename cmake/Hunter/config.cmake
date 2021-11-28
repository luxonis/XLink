# libusb without udev
hunter_config(
    libusb
    VERSION "1.0.24"
    URL "https://github.com/libusb/libusb/archive/v1.0.24.tar.gz"
    SHA1 "125ed27fa2590048ee47adbab930eb28c39fab09"
    CMAKE_ARGS
        EXTRA_FLAGS=--disable-udev
)
