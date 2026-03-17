cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK before the project() call so the toolchain is set.
include(pico_sdk_import.cmake)

project(picoos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK.
pico_sdk_init()

add_subdirectory(src)
