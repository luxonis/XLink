# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/57dc3a52a1ce09f617bd282c66c25485961e3361.tar.gz"
    SHA1 "098ee4702ceee9ff65bb4e1f69280b3eccc38c95"
    CMAKE_ARGS
        WITH_UDEV=OFF
)
