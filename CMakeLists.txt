# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
# With additions of Luxonis

cmake_minimum_required(VERSION 3.2)

include("cmake/HunterGate.cmake")
HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.23.258.tar.gz"
    SHA1 "062a19ab13ce8dffa9a882b6ce3e43bdabdf75d3"
    LOCAL # Local config for dependencies
)

set(TARGET_NAME "XLink")
project(${TARGET_NAME} LANGUAGES C CXX)

include(cmake/XLink.cmake)
include(cmake/Flags.cmake)

# Build examples
option(XLINK_BUILD_EXAMPLES "Build XLink examples" OFF)
# Build tests
option(XLINK_BUILD_TESTS "Build XLink tests" OFF)

add_library(${TARGET_NAME} ${XLINK_SOURCES})
add_flag_source(src/shared/XLinkDevice.c "-Werror=switch-enum")

# Add libusb dependency
hunter_add_package(luxonis-libusb)
find_package(usb-1.0 CONFIG REQUIRED)
target_link_libraries(${TARGET_NAME} PRIVATE usb-1.0)

if(WIN32)
    target_compile_definitions(${TARGET_NAME} PRIVATE WIN32_LEAN_AND_MEAN)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
endif()

target_include_directories(${TARGET_NAME}
    PRIVATE "${LIBUSB_INCLUDE_DIR}"
)
target_link_libraries(${TARGET_NAME}
    PUBLIC ${LIBUSB_LIBRARY}
)

if(MINGW)
    target_link_libraries(${TARGET_NAME}
        PUBLIC
            libwinusb.a
            libsetupapi.a
    )
endif()

target_include_directories(${TARGET_NAME}
    PUBLIC
        $<INSTALL_INTERFACE:include>
        "$<BUILD_INTERFACE:${XLINK_INCLUDE}>"
    PRIVATE
        ${XLINK_PLATFORM_INCLUDE}
        ${XLINK_PRIVATE_INCLUDE}
        ${XLINK_INCLUDE}/XLink
    )

target_compile_definitions(${TARGET_NAME}
    PRIVATE
        HAVE_STRUCT_TIMESPEC
        _CRT_SECURE_NO_WARNINGS
        USE_USB_VSC
        USE_TCP_IP
    PUBLIC
        __PC__
)

if (ENABLE_MYRIAD_NO_BOOT)
    target_compile_definitions(${TARGET_NAME}
        PRIVATE
        NO_BOOT)
endif()

# Set C99 standard
set_property(TARGET ${TARGET_NAME} PROPERTY C_STANDARD 99)
# Set compiler features (c++11), and disables extensions (g++11)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_STANDARD 11)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_EXTENSIONS OFF)
# Add interface transitive property (C++11)
if(${CMAKE_VERSION} VERSION_LESS "3.8.0")
    target_compile_features(${TARGET_NAME} INTERFACE cxx_range_for)
else()
    target_compile_features(${TARGET_NAME} INTERFACE cxx_std_11)
endif()

# Add flags
add_default_flags(${TARGET_NAME})

# Examples
if(XLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(XLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS ${TARGET_NAME}
    EXPORT "${TARGET_NAME}Config"
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(config_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${TARGET_NAME}")
export(TARGETS
        ${TARGET_NAME}
    FILE "${config_install_dir}/${TARGET_NAME}Config.cmake"
)
install(EXPORT
    "${TARGET_NAME}Config"
    DESTINATION "${config_install_dir}"
)
