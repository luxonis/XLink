include(CTest)
enable_testing()

function(xlink_add_test test_name test_src)
    get_filename_component(test_target ${test_src} NAME_WE)

    if(NOT TARGET ${test_target})
        add_executable(${test_target} ${test_src})
        target_link_libraries(${test_target} ${TARGET_NAME})
        target_include_directories(${test_target} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
        set_property(TARGET ${test_target} PROPERTY CXX_STANDARD 11)
        set_property(TARGET ${test_target} PROPERTY CXX_STANDARD_REQUIRED ON)
        set_property(TARGET ${test_target} PROPERTY CXX_EXTENSIONS OFF)

        if(WIN32)
            if(CMAKE_VERSION VERSION_LESS "3.21")
                message(WARNING "Automatic DLL copying not available")
            else()
                set(depthai_dll_libraries "$<TARGET_RUNTIME_DLLS:${test_target}>")
                add_custom_command(TARGET ${test_target} POST_BUILD COMMAND
                    "$<$<BOOL:${depthai_dll_libraries}>:${CMAKE_COMMAND};-E;copy_if_different;${depthai_dll_libraries};$<TARGET_FILE_DIR:${test_target}>>"
                    COMMAND_EXPAND_LISTS
                    VERBATIM
                )
            endif()
        endif()
    endif()

    add_test(NAME ${test_name} COMMAND "$<TARGET_FILE:${test_target}>" ${ARGN})
endfunction()

# multiple_open_stream_test.cpp:
# [num_streams] [port] [timeout_ms]
xlink_add_test(multiple_open_stream_test multiple_open_stream_test.cpp 16 11480 10000)

# multithreading_search_test.cpp:
# [timeout_ms]
xlink_add_test(multithreading_search_test multithreading_search_test.cpp 10000)

# multiple_connection_test.cpp:
# [num_connections] [base_port] [num_streams] [num_packets] [timeout_ms]
xlink_add_test(multiple_connection_test_single multiple_connection_test.cpp 1 12490 1 1 5000)
xlink_add_test(multiple_connection_test_small multiple_connection_test.cpp 2 12510 4 8 10000)
xlink_add_test(multiple_connection_test_medium multiple_connection_test.cpp 4 12530 8 32 15000)
xlink_add_test(multiple_connection_test_stress multiple_connection_test.cpp 16 12550 16 120 30000)

# connection_shutdown_test.cpp:
# [num_connections] [base_port] [timeout_ms]
xlink_add_test(connection_shutdown_test connection_shutdown_test.cpp 8 11520 5000)

# reset_reconnect_stress_test.cpp:
# [rounds] [base_port] [payload_mib] [max_jitter_ms] [write_repeat_count] [connect_timeout_ms] [client_hang_timeout_ms] [server_hang_timeout_ms] [timeout_ms]
xlink_add_test(reset_reconnect_stress_test reset_reconnect_stress_test.cpp 64 11690 20 25 1 15000 5000 5000 20000)
xlink_add_test(reset_reconnect_stress_test_no_jitter reset_reconnect_stress_test.cpp 32 11710 20 0 1 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_low_jitter reset_reconnect_stress_test.cpp 32 11730 20 5 1 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_default_jitter reset_reconnect_stress_test.cpp 64 11750 20 25 1 15000 5000 5000 20000)
xlink_add_test(reset_reconnect_stress_test_high_jitter reset_reconnect_stress_test.cpp 64 11770 20 75 1 15000 5000 5000 25000)
xlink_add_test(reset_reconnect_stress_test_payload_1m reset_reconnect_stress_test.cpp 24 11790 1 25 1 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_payload_8m reset_reconnect_stress_test.cpp 24 11810 8 25 1 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_payload_32m reset_reconnect_stress_test.cpp 32 11830 32 25 1 15000 15000 15000 20000)
xlink_add_test(reset_reconnect_stress_test_repeat_2x reset_reconnect_stress_test.cpp 24 11850 8 25 2 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_repeat_4x reset_reconnect_stress_test.cpp 16 11870 4 25 4 15000 5000 5000 10000)
xlink_add_test(reset_reconnect_stress_test_connect_5s reset_reconnect_stress_test.cpp 64 11890 20 25 1 5000 18000 18000 25000)

# resource_cleanup_test.cpp:
# [rounds] [port] [connect_timeout_ms] [warmup_rounds] [sample_every_rounds] [rss_growth_limit_kib] [fd_growth_limit] [server_grace_ms] [timeout_ms]
xlink_add_test(resource_cleanup_test_smoke resource_cleanup_test.cpp 50 11910 5000 10 10 24576 16 250 30000)

# rtt_test.cpp:
# [timeout_ms]
xlink_add_test(rtt_test rtt_test.cpp 15000)
# xlink_add_test(throughput_test throughput_test.cpp)
# xlink_add_test(connect_timeout_test connect_timeout_test.cpp 4 11930 500 5000)
