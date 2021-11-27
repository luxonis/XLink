set(XLINK_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../)

set(XLINK_INCLUDE ${XLINK_ROOT_DIR}/include)

set(XLINK_PRIVATE_INCLUDE ${XLINK_ROOT_DIR}/src/pc/protocols)

file(GLOB PC_SRC             "${XLINK_ROOT_DIR}/src/pc/*.c")
file(GLOB PC_PROTO_SRC       "${XLINK_ROOT_DIR}/src/pc/protocols/*.c")
file(GLOB_RECURSE SHARED_SRC "${XLINK_ROOT_DIR}/src/shared/*.c")

list(APPEND XLINK_SOURCES ${PC_SRC} ${PC_PROTO_SRC} ${SHARED_SRC})

if(WIN32)
    set(XLINK_PLATFORM_INCLUDE ${XLINK_ROOT_DIR}/src/pc/Win/include)

    file(GLOB XLINK_PLATFORM_SRC "${XLINK_ROOT_DIR}/src/pc/Win/src/*.c")

    if(MINGW)
        # Remove win_pthread.c, win_semaphore.c, and win_synchapi.c (use MinGW impl.)
        list(REMOVE_ITEM XLINK_PLATFORM_SRC "${XLINK_ROOT_DIR}/src/pc/Win/src/win_pthread.c")
        list(REMOVE_ITEM XLINK_PLATFORM_SRC "${XLINK_ROOT_DIR}/src/pc/Win/src/win_semaphore.c")
        list(REMOVE_ITEM XLINK_PLATFORM_SRC "${XLINK_ROOT_DIR}/src/pc/Win/src/win_synchapi.c")
    endif()

    list(APPEND XLINK_SOURCES "${XLINK_ROOT_DIR}/src/pc/protocols/usb_mx_id.c")
    list(APPEND XLINK_SOURCES ${XLINK_PLATFORM_SRC})
else()
    find_package(Threads REQUIRED)

    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES "include" "libusb" "libusb-1.0")
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 PATH_SUFFIXES "lib")

    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb is required")
    endif()

    set(XLINK_PLATFORM_INCLUDE "${XLINK_ROOT_DIR}/src/pc/MacOS" )
    list(APPEND XLINK_SOURCES "${XLINK_ROOT_DIR}/src/pc/MacOS/pthread_semaphore.c")
endif(WIN32)

#This is for the Movidius team
set(XLINK_INCLUDE_DIRECTORIES ${XLINK_INCLUDE} ${LIBUSB_INCLUDE_DIR})
