# Helper 'add_example'
macro(add_example example_name example_src)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} ${TARGET_NAME})
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${example_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${example_name} PROPERTY CXX_EXTENSIONS OFF)
endmacro()

# Examples

# List devices
add_example(list_devices list_devices.cpp)
