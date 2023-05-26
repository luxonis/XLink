# Helper 'add_example'
macro(add_example example_name example_src)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} ${TARGET_NAME})
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD 14)
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${example_name} PROPERTY CXX_EXTENSIONS OFF)

    # Copy over required DLLs (Windows)
    if(WIN32)
        # Copy dlls to target directory - Windows only
        # TARGET_RUNTIME_DLLS generator expression available since CMake 3.21
        if(CMAKE_VERSION VERSION_LESS "3.21")
            message(WARNING "Automatic DLL copying not available")
        else()
            set(depthai_dll_libraries "$<TARGET_RUNTIME_DLLS:${example_name}>")
            add_custom_command(TARGET ${example_name} POST_BUILD COMMAND
                ${CMAKE_COMMAND} -E copy_if_different ${depthai_dll_libraries} $<TARGET_FILE_DIR:${example_name}>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()
endmacro()

# Examples

# List devices
add_example(list_devices list_devices.cpp)

# Search devices
add_example(search_devices search_devices.cpp)

# Boot bootloader
add_example(boot_bootloader boot_bootloader.cpp)

# Boot firmware
add_example(boot_firmware boot_firmware.cpp)
