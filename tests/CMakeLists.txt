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
        ${CMAKE_COMMAND} -E env XLINK_STRESS_DUMP_ON_WATCHDOG=1
        ${BASH_EXECUTABLE} "${test_script}"
        "$<TARGET_FILE:${test_name}_server>"
        "$<TARGET_FILE:${test_name}_client>"
        "${num_iterations}"
        "${timeout}"
        COMMAND_EXPAND_LISTS
    )
endmacro()

macro(xlink_add_client_server_env_test test_name num_iterations timeout env_args)
    if(NOT TARGET ${test_name}_client)
        add_executable(${test_name}_client ${test_name}.cpp)
        target_compile_definitions(${test_name}_client PRIVATE XLINK_TEST_CLIENT)
        target_link_libraries(${test_name}_client ${TARGET_NAME})
    endif()

    if(NOT TARGET ${test_name}_server)
        add_executable(${test_name}_server ${test_name}.cpp)
        target_compile_definitions(${test_name}_server PRIVATE XLINK_TEST_SERVER)
        target_link_libraries(${test_name}_server ${TARGET_NAME})
    endif()

    if("${ARGV4}" STREQUAL "")
        set(test_script "${CMAKE_CURRENT_LIST_DIR}/bash/test_client_server.sh")
    else()
        set(test_script "${CMAKE_CURRENT_LIST_DIR}/bash/${ARGV4}")
    endif()

    add_test(NAME ${test_name}_${ARGV5} COMMAND
        ${CMAKE_COMMAND} -E env XLINK_STRESS_DUMP_ON_WATCHDOG=1 ${env_args}
        ${BASH_EXECUTABLE} "${test_script}"
        "$<TARGET_FILE:${test_name}_server>"
        "$<TARGET_FILE:${test_name}_client>"
        "${num_iterations}"
        "${timeout}"
        COMMAND_EXPAND_LISTS
    )
endmacro()

# Tests

# Multiple stream open (10 iterations)
xlink_add_client_server_test(multiple_open_stream_test 10 10)

# Multithreading search
xlink_add_test(multithreading_search_test multithreading_search_test.cpp)

# multiple_connection_test
xlink_add_client_server_test(multiple_connection_test 16 10 multiple_connection_test.sh)

# connection shutdown test
xlink_add_client_server_test(connection_shutdown_test 8 10)

# reset/connect/reconnect stress test
xlink_add_client_server_test(reset_reconnect_stress_test 64 8 reset_reconnect_stress_test.sh)
xlink_add_client_server_env_test(reset_reconnect_stress_test 32 8
    "XLINK_STRESS_JITTER_MS=0"
    reset_reconnect_stress_test.sh no_jitter)
xlink_add_client_server_env_test(reset_reconnect_stress_test 32 8
    "XLINK_STRESS_JITTER_MS=5"
    reset_reconnect_stress_test.sh low_jitter)
xlink_add_client_server_env_test(reset_reconnect_stress_test 64 8
    "XLINK_STRESS_JITTER_MS=25"
    reset_reconnect_stress_test.sh default_jitter)
xlink_add_client_server_env_test(reset_reconnect_stress_test 64 8
    "XLINK_STRESS_JITTER_MS=75"
    reset_reconnect_stress_test.sh high_jitter)
xlink_add_client_server_env_test(reset_reconnect_stress_test 24 8
    "XLINK_STRESS_PAYLOAD_MIB=1;XLINK_STRESS_JITTER_MS=25"
    reset_reconnect_stress_test.sh payload_1m)
xlink_add_client_server_env_test(reset_reconnect_stress_test 24 8
    "XLINK_STRESS_PAYLOAD_MIB=8;XLINK_STRESS_JITTER_MS=25"
    reset_reconnect_stress_test.sh payload_8m)
xlink_add_client_server_env_test(reset_reconnect_stress_test 32 12
    "XLINK_STRESS_PAYLOAD_MIB=32;XLINK_STRESS_JITTER_MS=25;XLINK_STRESS_CLIENT_HANG_TIMEOUT_MS=15000;XLINK_STRESS_SERVER_HANG_TIMEOUT_MS=15000"
    reset_reconnect_stress_test.sh payload_32m)
xlink_add_client_server_env_test(reset_reconnect_stress_test 24 8
    "XLINK_STRESS_WRITE_REPEAT_COUNT=2;XLINK_STRESS_PAYLOAD_MIB=8"
    reset_reconnect_stress_test.sh repeat_2x)
xlink_add_client_server_env_test(reset_reconnect_stress_test 16 8
    "XLINK_STRESS_WRITE_REPEAT_COUNT=4;XLINK_STRESS_PAYLOAD_MIB=4"
    reset_reconnect_stress_test.sh repeat_4x)
xlink_add_client_server_env_test(reset_reconnect_stress_test 64 20
    "XLINK_STRESS_CONNECT_TIMEOUT_MS=5000;XLINK_STRESS_JITTER_MS=25;XLINK_STRESS_CLIENT_HANG_TIMEOUT_MS=18000;XLINK_STRESS_SERVER_HANG_TIMEOUT_MS=18000"
    reset_reconnect_stress_test.sh connect_5s)

# resource cleanup / leak smoke test
xlink_add_client_server_env_test(resource_cleanup_test 500 10
    "XLINK_LEAK_TEST_WARMUP_ROUNDS=50;XLINK_LEAK_TEST_SAMPLE_EVERY_ROUNDS=50;XLINK_LEAK_TEST_CLIENT_TIMEOUT=300"
    resource_cleanup_test.sh smoke)

# RTT test
xlink_add_test(rtt_test rtt_test.cpp)

# # Throughput test
# xlink_add_test(throughput_test throughput_test.cpp)

# TODO(themarpe) - XLinkConnectWithTimeout doesn't work yet as expected
# # connect timeout test
# xlink_add_client_server_test(connect_timeout_test 16 10 connect_timeout_test.sh)
