# libusb without udev
hunter_config(
    luxonis-libusb
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/2ea8c929dd28db443f8b7e8cdc37b12476f80eac.tar.gz"
    SHA1 "e0f07065a9b232cd65d00434ee6eadd5d8c5028b"
    CMAKE_ARGS
        WITH_UDEV=OFF
)
