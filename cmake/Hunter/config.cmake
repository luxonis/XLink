# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/603ea0c74b6119c8fec25c20d4358462ee1309d4.tar.gz"
    SHA1 "46ed5287889efa4e78d23741facd70fb432d6914"
    CMAKE_ARGS
        WITH_UDEV=OFF
)
