# libusb without udev
hunter_config(
    luxonis-libusb
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/87129a4bc3ab25e40e68b4f01e83088af7cb60d4.tar.gz"
    SHA1 "91739111f9c83cd5544a89ce0674e81cb58e0ded"
    CMAKE_ARGS
        WITH_UDEV=OFF
)
