cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Board name aliases — map underscore-free names to SDK-canonical names.
# This must run before pico_sdk_import.cmake reads PICO_BOARD.
#
#   picow   → pico_w
#   pico2w  → pico2_w
#
# Usage: cmake -B builds/picow -DPICO_BOARD=picow   (not "build" — that is a script)
# ---------------------------------------------------------------------------
if(PICO_BOARD STREQUAL "picow")
    set(PICO_BOARD "pico_w" CACHE STRING "" FORCE)
elseif(PICO_BOARD STREQUAL "pico2w")
    set(PICO_BOARD "pico2_w" CACHE STRING "" FORCE)
endif()

# Pull in the Pico SDK before the project() call so the toolchain is set.
# Optimise for size by default (-Os).  The SDK would otherwise pick Release
# (-O3), which inlines and unrolls the kernel's pool loops: about 25% more
# flash for no benefit that matters here.  Must be set before the SDK import,
# which applies its own default.  Override with -DCMAKE_BUILD_TYPE=Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING
        "Choose the type of build, options are: 'Debug', 'Release', 'MinSizeRel', 'RelWithDebInfo'.")
endif()

include(pico_sdk_import.cmake)

project(picoos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK.
pico_sdk_init()

add_subdirectory(src)
