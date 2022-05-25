# Helper 'add_example'
macro(add_example example_name example_src)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} ${TARGET_NAME})
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${example_name} PROPERTY CXX_EXTENSIONS OFF)
endmacro()
macro(add_example_server example_name example_src)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} ${TARGET_NAME_SERVER})
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${example_name} PROPERTY CXX_EXTENSIONS OFF)
endmacro()
# Examples

# List devices
add_example(list_devices list_devices.cpp)

# Boot bootloader
add_example(boot_bootloader boot_bootloader.cpp)

# Boot firmware
add_example(boot_firmware boot_firmware.cpp)

# Server example
add_example_server(xlink_server xlink_server.cpp)