# Helper 'add_test'
macro(add_test test_name test_src)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${TARGET_NAME})
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${test_name} PROPERTY CXX_EXTENSIONS OFF)
endmacro()
macro(add_test_server test_name test_src)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${TARGET_NAME_SERVER})
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 11)
    set_property(TARGET ${test_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${test_name} PROPERTY CXX_EXTENSIONS OFF)
endmacro()

# Tests

# Multiple stream open
add_test(multiple_open_stream multiple_open_stream.cpp)
add_test_server(multiple_open_stream_server multiple_open_stream_server.cpp)
