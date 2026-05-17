/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service without prior written permission from the copyright holder.
 */

/*
 * tests/vfs/test_vfs.c — host-native tests for the picoOS VFS routing layer.
 *
 * The device layer (dev.c) and filesystem layer (fs.c) are replaced by the
 * stubs in mock_dev.c and mock_fs.c.  Each test calls vfs_reset() which
 * re-initialises the VFS tables and the mock call counters.
 *
 * Build (from tests/):
 *   cmake -B build && make -C build
 *   ./build/test_vfs
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "vfs.h"
#include "mock_dev.h"
#include "mock_fs.h"
#include "../framework.h"

/* Per-test reset: re-initialise the VFS and zero the mock counters. */
static void vfs_reset(void)
{
    mock_dev_reset();
    mock_fs_reset();
    vfs_init();          /* re-registers the 4 standard device mounts */
    /* Re-initialising resets mock call counters; clear again after vfs_init
     * because vfs_init calls dev_open implicitly via vfs_mount_dev calls. */
    mock_dev_reset();
    mock_fs_reset();
}

/* -------------------------------------------------------------------------
 * test_dev_open_routes_to_device_layer
 * Opening a registered device path must invoke the device layer.
 * ------------------------------------------------------------------------- */
static void test_dev_open_routes_to_device_layer(void)
{
    BEGIN_TEST(dev_open_calls_device_layer);
    vfs_reset();

    int fd = vfs_open("/dev/console", VFS_O_RDWR);
    CHECK(fd >= 0,                    "vfs_open of /dev/console must succeed");
    CHECK(mock_dev_open_calls == 1,   "dev_open must have been called once");
    CHECK(mock_last_dev_opened == DEV_CONSOLE,
          "dev_open must have been called with DEV_CONSOLE");
    if (fd >= 0) vfs_close(fd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_file_open_routes_to_fs_layer
 * Opening a non-device path must delegate to the filesystem layer.
 * ------------------------------------------------------------------------- */
static void test_file_open_routes_to_fs_layer(void)
{
    BEGIN_TEST(file_open_calls_fs_layer);
    vfs_reset();

    int fd = vfs_open("/myfile.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0,                  "vfs_open of a file path must succeed");
    CHECK(mock_fs_open_calls == 1,  "fs_open must have been called once");
    if (fd >= 0) vfs_close(fd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_fd_table_exhaustion
 * Opening VFS_MAX_OPEN file descriptors must succeed; the next must fail.
 * ------------------------------------------------------------------------- */
static void test_fd_table_exhaustion(void)
{
    BEGIN_TEST(fd_table_exhaustion);
    vfs_reset();

    int fds[VFS_MAX_OPEN];
    bool all_ok = true;
    for (int i = 0; i < (int)VFS_MAX_OPEN; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/file%d.txt", i);
        fds[i] = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY);
        if (fds[i] < 0) { all_ok = false; break; }
    }
    CHECK(all_ok, "opening VFS_MAX_OPEN fds must all succeed");

    int overflow = vfs_open("/extra.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(overflow < 0, "opening beyond VFS_MAX_OPEN must return -1");

    for (int i = 0; i < (int)VFS_MAX_OPEN; i++) {
        if (fds[i] >= 0) vfs_close(fds[i]);
    }

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_close_reclaims_fd
 * After filling the table and closing one fd, another open must succeed.
 * ------------------------------------------------------------------------- */
static void test_close_reclaims_fd(void)
{
    BEGIN_TEST(close_reclaims_fd_slot);
    vfs_reset();

    int fds[VFS_MAX_OPEN];
    for (int i = 0; i < (int)VFS_MAX_OPEN; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/reclaim%d.txt", i);
        fds[i] = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY);
    }

    /* Close the first fd. */
    vfs_close(fds[0]);
    fds[0] = -1;

    /* The freed slot must now be available. */
    int new_fd = vfs_open("/new.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(new_fd >= 0, "opening after closing one fd must succeed");

    if (new_fd >= 0) vfs_close(new_fd);
    for (int i = 1; i < (int)VFS_MAX_OPEN; i++) {
        if (fds[i] >= 0) vfs_close(fds[i]);
    }

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_invalid_fd_ops
 * Operations on invalid fds (-1, too-large) must return -1 and not crash.
 * ------------------------------------------------------------------------- */
static void test_invalid_fd_ops(void)
{
    BEGIN_TEST(invalid_fd_read_write_close_return_minus_one);
    vfs_reset();

    uint8_t buf[8] = {0};

    CHECK(vfs_read(-1, buf, 8u)  == -1, "vfs_read(-1) must return -1");
    CHECK(vfs_write(-1, buf, 8u) == -1, "vfs_write(-1) must return -1");
    CHECK(vfs_close(-1)          == -1, "vfs_close(-1) must return -1");

    int too_large = (int)VFS_MAX_OPEN;
    CHECK(vfs_read(too_large, buf, 8u)  == -1, "vfs_read(VFS_MAX_OPEN) must return -1");
    CHECK(vfs_write(too_large, buf, 8u) == -1, "vfs_write(VFS_MAX_OPEN) must return -1");
    CHECK(vfs_close(too_large)          == -1, "vfs_close(VFS_MAX_OPEN) must return -1");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_null_path_fails
 * vfs_open(NULL, ...) must return -1.
 * ------------------------------------------------------------------------- */
static void test_null_path_fails(void)
{
    BEGIN_TEST(null_path_open_returns_minus_one);
    vfs_reset();

    int fd = vfs_open(NULL, VFS_O_RDONLY);
    CHECK(fd < 0, "vfs_open(NULL) must return -1");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_double_close_fails
 * Closing the same fd twice: the second close must return -1.
 * ------------------------------------------------------------------------- */
static void test_double_close_fails(void)
{
    BEGIN_TEST(double_close_second_returns_minus_one);
    vfs_reset();

    int fd = vfs_open("/dbl.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0, "open must succeed");

    int r1 = vfs_close(fd);
    CHECK(r1 == 0, "first close must return 0");

    int r2 = vfs_close(fd);
    CHECK(r2 == -1, "second close of the same fd must return -1");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_dev_close_routes_to_device_layer
 * Closing a device fd must invoke the device layer's close.
 * ------------------------------------------------------------------------- */
static void test_dev_close_routes_to_device_layer(void)
{
    BEGIN_TEST(dev_close_calls_device_layer);
    vfs_reset();

    int fd = vfs_open("/dev/timer", VFS_O_RDONLY);
    CHECK(fd >= 0, "open must succeed");
    int before = mock_dev_close_calls;

    vfs_close(fd);
    CHECK(mock_dev_close_calls == before + 1,
          "dev_close must be called once on vfs_close of a device fd");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_file_close_routes_to_fs_layer
 * Closing a filesystem fd must invoke the fs layer's close.
 * ------------------------------------------------------------------------- */
static void test_file_close_routes_to_fs_layer(void)
{
    BEGIN_TEST(file_close_calls_fs_layer);
    vfs_reset();

    int fd = vfs_open("/close_me.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0, "open must succeed");
    int before = mock_fs_close_calls;

    vfs_close(fd);
    CHECK(mock_fs_close_calls == before + 1,
          "fs_close must be called once on vfs_close of a file fd");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_mount_table_exhaustion
 * vfs_init() registers 4 devices.  Adding VFS_MAX_DEV_MOUNTS - 4 more must
 * succeed; adding one beyond the limit must fail.
 * ------------------------------------------------------------------------- */
static void test_mount_table_exhaustion(void)
{
    BEGIN_TEST(mount_table_exhaustion);
    vfs_reset();   /* 4 slots already used by vfs_init's standard mounts */

    /* VFS_MAX_DEV_MOUNTS is 8; 4 are already taken.  Mount 4 more. */
    bool extra_ok = true;
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/extra%d", i);
        int rc = vfs_mount_dev(path, DEV_CONSOLE);
        if (rc != 0) { extra_ok = false; break; }
    }
    CHECK(extra_ok, "filling mount table to VFS_MAX_DEV_MOUNTS must succeed");

    /* One more must fail. */
    int overflow = vfs_mount_dev("/dev/toomany", DEV_CONSOLE);
    CHECK(overflow < 0, "mounting beyond VFS_MAX_DEV_MOUNTS must return -1");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("picoOS VFS routing layer — unit tests\n");
    printf("=======================================\n\n");

    test_dev_open_routes_to_device_layer();
    test_file_open_routes_to_fs_layer();
    test_fd_table_exhaustion();
    test_close_reclaims_fd();
    test_invalid_fd_ops();
    test_null_path_fails();
    test_double_close_fails();
    test_dev_close_routes_to_device_layer();
    test_file_close_routes_to_fs_layer();
    test_mount_table_exhaustion();

    SUMMARY();
}
