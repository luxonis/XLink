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
macro(xlink_add_client_server_test test_name num_iterations)
    # Add client
    add_executable(${test_name}_client ${test_name}.cpp)
    target_compile_definitions(${test_name}_client PRIVATE XLINK_TEST_CLIENT)
    target_link_libraries(${test_name}_client ${TARGET_NAME})

    # Add server
    add_executable(${test_name}_server ${test_name}.cpp)
    target_compile_definitions(${test_name}_server PRIVATE XLINK_TEST_SERVER)
    target_link_libraries(${test_name}_server ${TARGET_NAME})

    # Add to test suite
    add_test(NAME ${test_name} COMMAND
        ${CMAKE_COMMAND}
        "-D CLIENT_EXECUTABLE=$<TARGET_FILE:${test_name}_client>"
        "-D SERVER_EXECUTABLE=$<TARGET_FILE:${test_name}_server>"
        "-D NUM_ITERATIONS=${num_iterations}"
        "-P ${CMAKE_CURRENT_LIST_DIR}/cmake/test_client_server.cmake"
        COMMAND_EXPAND_LISTS
    )
endmacro()

# Tests

# Multiple stream open (10 iterations)
xlink_add_client_server_test(multiple_open_stream_test 10)
# Multithreading search
xlink_add_test(multithreading_search_test multithreading_search_test.cpp)

# multiple_connection_test search
xlink_add_test(multiple_connection_test_server multiple_connection_test.cpp)
target_compile_definitions(multiple_connection_test_server PRIVATE XLINK_TEST_SERVER)
xlink_add_test(multiple_connection_test_client multiple_connection_test.cpp)
target_compile_definitions(multiple_connection_test_client PRIVATE XLINK_TEST_CLIENT)

# connection shutdown
xlink_add_test(connection_shutdown_test_server connection_shutdown_test.cpp)
target_compile_definitions(connection_shutdown_test_server PRIVATE XLINK_TEST_SERVER)
xlink_add_test(connection_shutdown_test_client connection_shutdown_test.cpp)
target_compile_definitions(connection_shutdown_test_client PRIVATE XLINK_TEST_CLIENT)

# connect timeout test
xlink_add_test(connect_timeout_test connect_timeout_test.cpp)