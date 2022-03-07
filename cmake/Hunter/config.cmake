# libusb without udev
hunter_config(
    libusb-luxonis
    VERSION "1.0.24-cmake"
    URL "https://github.com/luxonis/libusb/archive/b481f0d19499da513802259b96e433245306199b.tar.gz"
    SHA1 "7b74e3850ef99fed7bb27107592a2906d1d99b9b"
    CMAKE_ARGS
        WITH_UDEV=OFF
)
