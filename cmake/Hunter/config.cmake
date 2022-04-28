# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/4a5f684d4b1ddfb462468056b31290d674b340b1.tar.gz"
    SHA1 "fd741a5d27ac954a4a7e4876fddc0d98bac83d01"
    CMAKE_ARGS
        WITH_UDEV=OFF
        # Build shared libs by default to not cause licensing issues
        BUILD_SHARED_LIBS=ON
)
