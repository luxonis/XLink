if(CONFIG_MODE)
    set(_CMAKE_PREFIX_PATH_ORIGINAL ${CMAKE_PREFIX_PATH})
    set(_CMAKE_FIND_ROOT_PATH_MODE_PACKAGE_ORIGINAL ${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE})
    # Fixes Android NDK build, where prefix path is ignored as its not inside sysroot
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE "BOTH")
    set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/${_IMPORT_PREFIX}" ${CMAKE_PREFIX_PATH})
    set(_QUIET "QUIET")
else()
    # set(XLINK_SHARED_LIBS ${BUILD_SHARED_LIBS})
    if(NOT XLINK_LIBUSB_LOCAL AND NOT XLINK_LIBUSB_SYSTEM)
        hunter_add_package(libusb-luxonis)
    endif()
endif()

# libusb
if(XLINK_LIBUSB_LOCAL)
    add_subdirectory("${XLINK_LIBUSB_LOCAL}" "${CMAKE_CURRENT_BINARY_DIR}/libusb" EXCLUDE_FROM_ALL)
elseif(NOT XLINK_LIBUSB_SYSTEM)
    find_package(usb-1.0 ${_QUIET} CONFIG REQUIRED HINTS "${CMAKE_CURRENT_LIST_DIR}/libusb")
endif()

# Add threads (c++)
find_package(Threads ${_QUIET} REQUIRED)

# include optional dependency cmake
if(XLINK_DEPENDENCY_INCLUDE)
    include(${XLINK_DEPENDENCY_INCLUDE} OPTIONAL)
endif()

# Cleanup
if(CONFIG_MODE)
    set(CMAKE_PREFIX_PATH ${_CMAKE_PREFIX_PATH_ORIGINAL})
    set(_CMAKE_PREFIX_PATH_ORIGINAL)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ${_CMAKE_FIND_ROOT_PATH_MODE_PACKAGE_ORIGINAL})
    set(_CMAKE_FIND_ROOT_PATH_MODE_PACKAGE_ORIGINAL)
    set(_QUIET)
else()
    set(XLINK_SHARED_LIBS)
endif()
