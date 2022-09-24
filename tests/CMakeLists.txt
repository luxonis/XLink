# Add CMake testing infrastructure
include(CTest)
enable_testing()

# Helper 'xlink_add_test'
macro(xlink_add_test test_name test_src)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${TARGET_NAME})
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${test_name} PROPERTY CXX_EXTENSIONS OFF)
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
        bash "${test_script}"
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

# connect timeout test
xlink_add_client_server_test(connect_timeout_test 16 10 connect_timeout_test.sh)
