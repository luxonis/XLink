# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
# With additions of Luxonis

cmake_minimum_required(VERSION 3.10)

include("cmake/HunterGate.cmake")
HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/9d9242b60d5236269f894efd3ddd60a9ca83dd7f.tar.gz"
    SHA1 "16cc954aa723bccd16ea45fc91a858d0c5246376"
    LOCAL # Local config for dependencies
)

### Constants
set(TARGET_NAME "XLink")
set(TARGET_PUBLIC_NAME "XLinkPublic")

# Create project
project(${TARGET_NAME} LANGUAGES C CXX)

set(PROJECT_EXPORT_GROUP "${PROJECT_NAME}Targets")

# Set default installation directory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Installation Directory" FORCE)
endif()

# Compile with USB protocol capabilities
option(XLINK_ENABLE_LIBUSB "Enable USB protocol which requires libusb library" ON)
# Build examples
option(XLINK_BUILD_EXAMPLES "Build XLink examples" OFF)
# Build tests
option(XLINK_BUILD_TESTS "Build XLink tests" OFF)
# Debug option
set(XLINK_LIBUSB_LOCAL "" CACHE STRING "Path to local libub source to use instead of Hunter")
# Debug option
option(XLINK_LIBUSB_SYSTEM "Use system libusb library instead of Hunter" OFF)

# Specify exporting all symbols on Windows
if(WIN32 AND BUILD_SHARED_LIBS)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON CACHE BOOL "")
endif()

if(XLINK_LIBUSB_LOCAL AND XLINK_LIBUSB_SYSTEM)
    message(FATAL_ERROR "XLINK_LIBUSB_LOCAL and XLINK_LIBUSB_SYSTEM options are mutually exclusive!")
endif()

message(STATUS "Configuring XLink with the following options:")
message(STATUS "  XLINK_BUILD_EXAMPLES: ${XLINK_BUILD_EXAMPLES}")
message(STATUS "  XLINK_BUILD_TESTS: ${XLINK_BUILD_TESTS}")
message(STATUS "  XLINK_ENABLE_LIBUSB: ${XLINK_ENABLE_LIBUSB}")
if(XLINK_ENABLE_LIBUSB)
    message(STATUS "    XLINK_LIBUSB_LOCAL: ${XLINK_LIBUSB_LOCAL}")
    message(STATUS "    XLINK_LIBUSB_SYSTEM: ${XLINK_LIBUSB_SYSTEM}")
endif()

# Include XLink sources & flags helpers
include(cmake/XLink.cmake)
include(cmake/Flags.cmake)

# Create library target
add_library(${TARGET_NAME} ${XLINK_SOURCES})
add_flag_source(src/shared/XLinkDevice.c "-Werror=switch-enum")
get_cmake_property(is_multi_config GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
    set_target_properties(${TARGET_NAME} PROPERTIES DEBUG_POSTFIX "d")
endif()
# Define export header to support shared library on Windows
include(GenerateExportHeader)
generate_export_header(${TARGET_NAME}
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}Export.h)
# Add dependencies
include(cmake/XLinkDependencies.cmake)

if(XLINK_ENABLE_LIBUSB)
    if(XLINK_LIBUSB_SYSTEM)
        # Find system libusb
        find_path(LIBUSB_INCLUDE_DIR NAMES "libusb-1.0/libusb.h")
        find_library(LIBUSB_LIBRARY NAMES usb-1.0 PATH_SUFFIXES "lib")
        if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
            message(FATAL_ERROR "libusb is required")
        endif()
        target_include_directories(${TARGET_NAME}
            PRIVATE "${LIBUSB_INCLUDE_DIR}"
        )
        target_link_libraries(${TARGET_NAME}
            PUBLIC ${LIBUSB_LIBRARY}
        )
    else()
        # Link to CMake libusb
        target_link_libraries(${TARGET_NAME} PRIVATE usb-1.0)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_link_libraries(${TARGET_NAME} PRIVATE delayimp.lib)
            # workaround https://gitlab.kitware.com/cmake/cmake/-/issues/20022
	    target_link_options(${TARGET_NAME} PUBLIC "$<LINK_ONLY:/DELAYLOAD:libusb-1.0$<$<CONFIG:Debug>:d>.dll>")
        endif()
    endif()
    
    if(WIN32 AND NOT MINGW)
        target_link_libraries(${TARGET_NAME} PRIVATE Pathcch.lib)
    endif()

    # Add compile define stating a local libusb usage
    if(XLINK_LIBUSB_LOCAL)
        target_compile_definitions(${TARGET_NAME} PRIVATE XLINK_LIBUSB_LOCAL)
    endif()

    # Add compile define stating libusb is enabled
    target_compile_definitions(${TARGET_NAME} PRIVATE XLINK_ENABLE_LIBUSB)
endif()

if(WIN32)
    target_compile_definitions(${TARGET_NAME} PRIVATE WIN32_LEAN_AND_MEAN)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
endif()

if(MINGW)
    target_link_libraries(${TARGET_NAME}
        PUBLIC
            libwinusb.a
            libsetupapi.a
            ws2_32
            Iphlpapi.lib
    )
endif()

if(ANDROID)
    find_library(log-lib log)
    target_link_libraries(${TARGET_NAME} PRIVATE ${log-lib})
endif()

# Create public "interface" library (no linking)
add_library(${TARGET_PUBLIC_NAME} INTERFACE)
target_include_directories(${TARGET_PUBLIC_NAME} INTERFACE
    "$<INSTALL_INTERFACE:include>"
    "$<BUILD_INTERFACE:${XLINK_INCLUDE}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
)

# Link to headers (public, as itself also needs the headers)
target_link_libraries(${TARGET_NAME} PUBLIC ${TARGET_PUBLIC_NAME})

target_include_directories(${TARGET_NAME}
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
# Add interface transitive property (C++11) to public library
if(${CMAKE_VERSION} VERSION_LESS "3.8.0")
    target_compile_features(${TARGET_PUBLIC_NAME} INTERFACE cxx_range_for)
else()
    target_compile_features(${TARGET_PUBLIC_NAME} INTERFACE cxx_std_11)
endif()

# Add flags
add_default_flags(${TARGET_NAME})

# Check if pthread_getname_np exists
if(NOT WIN32)
    include(CheckSymbolExists)
    set(_TMP_CMAKE_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
    set(_TMP_CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    set(_TMP_CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    list(APPEND CMAKE_REQUIRED_LIBRARIES "-lpthread")
    # This flag breaks check_symbol_exists()
    string(REPLACE " -Werror " " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

    # perfom check for pthread_getname_np symbol
    check_symbol_exists(pthread_getname_np pthread.h HAVE_PTHREAD_GETNAME_NP)

    set(CMAKE_REQUIRED_DEFINITIONS "${_TMP_CMAKE_REQUIRED_DEFINITIONS}")
    set(CMAKE_REQUIRED_LIBRARIES "${_TMP_CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_C_FLAGS "${_TMP_CMAKE_C_FLAGS}")
    unset(_TMP_CMAKE_REQUIRED_DEFINITIONS)
    unset(_TMP_CMAKE_REQUIRED_LIBRARIES)
    unset(_TMP_CMAKE_C_FLAGS)

    # Set the define
    if(HAVE_PTHREAD_GETNAME_NP)
        target_compile_definitions(${TARGET_NAME} PRIVATE HAVE_PTHREAD_GETNAME_NP)
    endif()
endif()

# Examples
if(XLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(XLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Installation

include(GNUInstallDirs)

# Export targets (capability to import current build directory)
export(TARGETS ${TARGET_NAME} ${TARGET_PUBLIC_NAME} FILE "${PROJECT_NAME}Targets.cmake")

# Dependencies file
configure_file("cmake/${PROJECT_NAME}Dependencies.cmake" ${PROJECT_NAME}Dependencies.cmake COPYONLY)

# Configure config file (one for exporting build directory, one for installation)
if(${HUNTER_INSTALL_PREFIX})
    file(RELATIVE_PATH XLINK_DEPENDENCIES_INSTALLATION_PATH_REL "${CMAKE_CURRENT_BINARY_DIR}" "${HUNTER_INSTALL_PREFIX}")
endif()

configure_file(cmake/${PROJECT_NAME}Config.cmake.in ${PROJECT_NAME}Config.cmake @ONLY)

# Config for installation
set(XLINK_DEPENDENCIES_INSTALLATION_PATH_REL "./dependencies")
configure_file(cmake/${PROJECT_NAME}Config.cmake.in _install/${PROJECT_NAME}Config.cmake @ONLY)

# Install targets
install(
    TARGETS ${TARGET_NAME} ${TARGET_PUBLIC_NAME}
    EXPORT ${PROJECT_EXPORT_GROUP}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
#Install include folder
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Install generated export header
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}Export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/XLink
)

# Install Hunter dependencies
if(XLINK_ENABLE_LIBUSB)
    if(NOT XLINK_LIBUSB_LOCAL AND NOT XLINK_LIBUSB_SYSTEM)
        install(DIRECTORY "${HUNTER_INSTALL_PREFIX}/" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/dependencies")
    endif()
endif()
# Install export group (information about targets)
install(EXPORT ${PROJECT_EXPORT_GROUP}
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
# Install CMake specific files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/_install/${PROJECT_NAME}Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Dependencies.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
