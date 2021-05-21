# Copyright (C) 2018-2020 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#
cmake_minimum_required(VERSION 3.2)
set(TARGET_NAME "XLink")
project(${TARGET_NAME} LANGUAGES C CXX)

include(cmake/XLink.cmake)
include(cmake/Flags.cmake)

# Add option - MX ID naming instead of usb port
option(XLINK_USE_MX_ID_NAME "Use MX ID name instead of usb port" ON)
# Build examples
option(XLINK_BUILD_EXAMPLES "Build XLink examples" OFF)

add_library(${TARGET_NAME} ${XLINK_SOURCES})
add_flag_source(src/shared/XLinkDevice.c "-Werror=switch-enum")

if(NOT WIN32)
    target_include_directories(${TARGET_NAME}
            PRIVATE
            "${LIBUSB_INCLUDE_DIR}")

    target_link_libraries(${TARGET_NAME}
            PUBLIC
            Threads::Threads
            ${LIBUSB_LIBRARY})
endif()

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
    PUBLIC
        __PC__
        XLINK_USE_MX_ID_NAME=${XLINK_USE_MX_ID_NAME}
)

if (ENABLE_MYRIAD_NO_BOOT)
    target_compile_definitions(${TARGET_NAME}
        PRIVATE
        NO_BOOT)
endif()

set_property(TARGET ${TARGET_NAME} PROPERTY C_STANDARD 99)

# Examples
if(XLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
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
