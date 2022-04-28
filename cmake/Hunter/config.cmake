# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/f52d62ae4216505558277aa6ea34e8fbca5804c7.tar.gz"
    SHA1 "307ab83b0024df10800c60c246d9b2eca3b4f6b0"
    CMAKE_ARGS
        WITH_UDEV=OFF
        # Build shared libs by default to not cause licensing issues
        BUILD_SHARED_LIBS=ON
)
