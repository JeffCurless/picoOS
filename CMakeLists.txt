cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Board name aliases — map underscore-free names to SDK-canonical names.
# This must run before pico_sdk_import.cmake reads PICO_BOARD.
#
#   picow   → pico_w
#   pico2w  → pico2_w
#
# Usage: cmake -B build -DPICO_BOARD=picow
# ---------------------------------------------------------------------------
if(PICO_BOARD STREQUAL "picow")
    set(PICO_BOARD "pico_w" CACHE STRING "" FORCE)
elseif(PICO_BOARD STREQUAL "pico2w")
    set(PICO_BOARD "pico2_w" CACHE STRING "" FORCE)
endif()

# Pull in the Pico SDK before the project() call so the toolchain is set.
include(pico_sdk_import.cmake)

project(picoos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK.
pico_sdk_init()

add_subdirectory(src)
