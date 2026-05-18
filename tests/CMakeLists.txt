# Add CMake testing infrastructure
include(CTest)
enable_testing()

# Search for Bash executable
find_program(BASH_EXECUTABLE name "bash"
    HINTS "C:/Program Files/Git/bin" # Windows hint
)

# Helper 'xlink_add_test'
macro(xlink_add_test test_name test_src)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${TARGET_NAME})
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 11)
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
                "$<$<BOOL:${depthai_dll_libraries}>:${CMAKE_COMMAND};-E;copy_if_different;${depthai_dll_libraries};$<TARGET_FILE_DIR:${test_name}>>"
                COMMAND_EXPAND_LISTS
                VERBATIM
            )
        endif()
    endif()

    # Add to test suite
    add_test(NAME ${test_name} COMMAND
        "$<TARGET_FILE:${test_name}>"
        COMMAND_EXPAND_LISTS
    )

endmacro()

# Helper 'xlink_add_client_server_test'
macro(xlink_add_client_server_test test_name num_iterations timeout)
    # Add client
    add_executable(${test_name}_client ${test_name}.cpp)
    target_compile_definitions(${test_name}_client PRIVATE XLINK_TEST_CLIENT)
    target_link_libraries(${test_name}_client ${TARGET_NAME})

    # Add server
    add_executable(${test_name}_server ${test_name}.cpp)
    target_compile_definitions(${test_name}_server PRIVATE XLINK_TEST_SERVER)
    target_link_libraries(${test_name}_server ${TARGET_NAME})

    # Specify test script
    if("${ARGV3}" STREQUAL "")
        set(test_script "${CMAKE_CURRENT_LIST_DIR}/bash/test_client_server.sh")
    else()
        set(test_script "${CMAKE_CURRENT_LIST_DIR}/bash/${ARGV3}")
    endif()

    # Add to test suite
    add_test(NAME ${test_name} COMMAND
        ${BASH_EXECUTABLE} "${test_script}"
        "$<TARGET_FILE:${test_name}_server>"
        "$<TARGET_FILE:${test_name}_client>"
        "${num_iterations}"
        "${timeout}"
        COMMAND_EXPAND_LISTS
    )
endmacro()

# Tests

# Multithreading search
xlink_add_test(multithreading_search_test multithreading_search_test.cpp)

# RTT test
xlink_add_test(rtt_test rtt_test.cpp)

# WIP
# Throughput test
# xlink_add_test(throughput_test throughput_test.cpp)
