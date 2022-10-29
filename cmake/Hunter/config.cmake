# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/abfdd6a7f904521a320702bc965fc2af0c47677e.tar.gz"
    SHA1 "0a73da678d871d51316ed924a98b028f3f535ce8"
    CMAKE_ARGS
        WITH_UDEV=OFF
        # Build shared libs by default to not cause licensing issues
        BUILD_SHARED_LIBS=ON
)
