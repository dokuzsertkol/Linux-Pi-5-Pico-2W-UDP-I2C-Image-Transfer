
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED BUILD_TARGET)
    set(BUILD_TARGET "pi5" CACHE STRING "Build target")
endif()

if(BUILD_TARGET STREQUAL "pico2w")
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()

project(PicoNetwork LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(BUILD_TARGET STREQUAL "pico2w")

    pico_sdk_init()

    add_subdirectory(pico2w)

elseif(BUILD_TARGET STREQUAL "pi5")

    add_subdirectory(shared)
    add_subdirectory(pi5)

elseif(BUILD_TARGET STREQUAL "server")

    add_subdirectory(shared)
    add_subdirectory(server)

else()

    message(FATAL_ERROR "Unknown BUILD_TARGET: ${BUILD_TARGET}")

endif()
