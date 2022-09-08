
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(isMultiConfig)
    list(APPEND CMAKE_CONFIGURATION_TYPES Asan)
    list(APPEND CMAKE_CONFIGURATION_TYPES Tsan)
else()
    set(allowedBuildTypes Asan Tsan Debug Release RelWithDebInfo MinSizeRel)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${allowedBuildTypes}")
endif()

set(CMAKE_C_FLAGS_ASAN
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C compiler for Asan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_ASAN
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C++ compiler for Asan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address" CACHE STRING
    "Linker flags to be used to create executables for Asan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address" CACHE STRING
    "Linker lags to be used to create shared libraries for Asan build type." FORCE)

set(CMAKE_C_FLAGS_TSAN
"${CMAKE_C_FLAGS_DEBUG} -fsanitize=thread -fno-omit-frame-pointer" CACHE STRING
"Flags used by the C compiler for Asan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_TSAN
"${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=thread -fno-omit-frame-pointer" CACHE STRING
"Flags used by the C++ compiler for Asan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_TSAN
"${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=thread" CACHE STRING
"Linker flags to be used to create executables for Asan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_TSAN
"${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=thread" CACHE STRING
"Linker lags to be used to create shared libraries for Asan build type." FORCE)

