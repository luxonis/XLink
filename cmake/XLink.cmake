set(XLINK_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../)

set(XLINK_INCLUDE ${XLINK_ROOT_DIR}/include)

set(XLINK_PRIVATE_INCLUDE ${XLINK_ROOT_DIR}/src/pc/protocols)

file(GLOB PC_SRC             "${XLINK_ROOT_DIR}/src/pc/*.c")
file(GLOB PC_SRC_CPP         "${XLINK_ROOT_DIR}/src/pc/*.cpp")
file(GLOB PC_PROTO_SRC       "${XLINK_ROOT_DIR}/src/pc/protocols/*.c")
file(GLOB PC_PROTO_SRC_CPP   "${XLINK_ROOT_DIR}/src/pc/protocols/*.cpp")
file(GLOB_RECURSE SHARED_SRC "${XLINK_ROOT_DIR}/src/shared/*.c")

list(APPEND XLINK_SOURCES ${PC_SRC} ${PC_SRC_CPP} ${PC_PROTO_SRC} ${PC_PROTO_SRC_CPP} ${SHARED_SRC})

if(WIN32)
    set(XLINK_PLATFORM_INCLUDE ${XLINK_ROOT_DIR}/src/pc/Win/include)

    file(GLOB XLINK_PLATFORM_SRC "${XLINK_ROOT_DIR}/src/pc/Win/src/*.c")
    file(GLOB XLINK_PLATFORM_SRC_CPP "${XLINK_ROOT_DIR}/src/pc/Win/src/*.cpp")
    list(APPEND XLINK_SOURCES ${XLINK_PLATFORM_SRC} ${XLINK_PLATFORM_SRC_CPP})
endif()

if(APPLE)
    set(XLINK_PLATFORM_INCLUDE "${XLINK_ROOT_DIR}/src/pc/MacOS" )
    list(APPEND XLINK_SOURCES "${XLINK_ROOT_DIR}/src/pc/MacOS/pthread_semaphore.c")
endif()

