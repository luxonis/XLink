# Helper 'add_test'
macro(add_test test_name test_src)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${TARGET_NAME})
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 14)
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${test_name} PROPERTY CXX_EXTENSIONS OFF)

    # Copy over required DLLs (Windows)
    if(WIN32)
        # Copy dlls to target directory - Windows only
        # TARGET_RUNTIME_DLLS generator expression available since CMake 3.21
        if(CMAKE_VERSION VERSION_LESS "3.21")
            message(WARNING "Automatic DLL copying not available")
        else()
            set(depthai_dll_libraries "$<TARGET_RUNTIME_DLLS:${test_name}>")
            add_custom_command(TARGET ${test_name} POST_BUILD COMMAND
                ${CMAKE_COMMAND} -E copy_if_different ${depthai_dll_libraries} $<TARGET_FILE_DIR:${test_name}>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()
endmacro()

# Tests

# Multiple stream open
add_test(multiple_open_stream multiple_open_stream.cpp)

# Multithreading search
add_test(multithreading_search_test multithreading_search_test.cpp)