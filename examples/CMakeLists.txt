# List devices
add_executable(list_devices list_devices.cpp)
target_link_libraries(list_devices ${TARGET_NAME})

# Boot bootloader
add_executable(boot_bootloader boot_bootloader.cpp)
target_link_libraries(boot_bootloader ${TARGET_NAME})
