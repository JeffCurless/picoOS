# ---------------------------------------------------------------------------
# picoOS host-native tests
#
# These tests compile kernel subsystem sources directly against the host
# toolchain — no Pico SDK or hardware required.
#
# Build and run:
#   cd tests
#   cmake -B build
#   make -C build
#   ctest --test-dir build --output-on-failure
#
# Or from the project root:
#   ./build tests
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.13)
project(picoos_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(KERNEL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/kernel)

# Common compile flags shared by all test targets.
set(TEST_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Werror)

# ---------------------------------------------------------------------------
# Memory allocator tests
# ---------------------------------------------------------------------------
add_executable(test_mem
    mem/test_mem.c
    ${KERNEL_SRC}/mem.c
)
target_include_directories(test_mem PRIVATE ${KERNEL_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_mem PRIVATE ${TEST_FLAGS})
add_test(NAME mem COMMAND test_mem)

# ---------------------------------------------------------------------------
# Sync primitives tests (spinlock, mutex, semaphore, event flags, mqueue)
#
# sched.c is replaced by sync/mock_sched.c so the scheduler is a no-op.
# Tests cover only non-blocking paths (sched_yield is a no-op on the host).
# ---------------------------------------------------------------------------
add_executable(test_sync
    sync/test_sync.c
    sync/mock_sched.c
    ${KERNEL_SRC}/sync.c
)
target_include_directories(test_sync PRIVATE ${KERNEL_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_sync PRIVATE ${TEST_FLAGS})
add_test(NAME sync COMMAND test_sync)

# ---------------------------------------------------------------------------
# Filesystem tests
#
# Flash hardware (XIP reads, erase, program, multicore lockout) is replaced
# by fs/mock_flash.c which uses a malloc'd RAM buffer.  The HOST_TEST
# compile definition activates the arch.h HOST_TEST block so that XIP_BASE
# resolves to the mock buffer address and the flash function stubs in arch.h
# are suppressed (letting mock_flash.c provide them instead).
#
# FS_MAX_FILES=8 limits the mock buffer to ~36 KB.
# FS_FLASH_SIZE is set conservatively to cover the test filesystem region.
# ---------------------------------------------------------------------------
add_executable(test_fs
    fs/test_fs.c
    fs/mock_flash.c
    ${KERNEL_SRC}/fs.c
)
target_include_directories(test_fs PRIVATE
    ${KERNEL_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fs
)
target_compile_definitions(test_fs PRIVATE
    HOST_TEST
    FS_MAX_FILES=8
    FS_FLASH_SIZE=40960
)
target_compile_options(test_fs PRIVATE ${TEST_FLAGS})
add_test(NAME fs COMMAND test_fs)

# ---------------------------------------------------------------------------
# VFS routing layer tests
#
# dev.c and fs.c are replaced by vfs/mock_dev.c and vfs/mock_fs.c.
# Tests verify that vfs.c routes paths and delegates calls correctly without
# any real device or flash hardware.
# ---------------------------------------------------------------------------
add_executable(test_vfs
    vfs/test_vfs.c
    vfs/mock_dev.c
    vfs/mock_fs.c
    ${KERNEL_SRC}/vfs.c
)
target_include_directories(test_vfs PRIVATE
    ${KERNEL_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs
)
target_compile_options(test_vfs PRIVATE ${TEST_FLAGS})
add_test(NAME vfs COMMAND test_vfs)
