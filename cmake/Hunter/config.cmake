# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/b7e4548958325b18feb73977163ad44398099534.tar.gz"
    SHA1 "2d79573d57628fe56d2868d2f6ce756d40906cf4"
    CMAKE_ARGS
        WITH_UDEV=OFF
        # Build shared libs by default to not cause licensing issues
        BUILD_SHARED_LIBS=ON
)
