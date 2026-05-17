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
 * tests/vfs/mock_fs.c — filesystem-layer stub for host-native vfs tests.
 *
 * fs_open() returns incrementing fd values so vfs.c can store and pass them
 * back.  Call counters let tests verify that vfs.c delegates to the fs layer.
 */

#include "mock_fs.h"
#include "fs.h"
#include <stddef.h>

int mock_fs_open_calls  = 0;
int mock_fs_close_calls = 0;
int mock_fs_next_fd     = 0;

void mock_fs_reset(void)
{
    mock_fs_open_calls  = 0;
    mock_fs_close_calls = 0;
    mock_fs_next_fd     = 0;
}

void fs_init(void) {}

int fs_open(const char *name, int mode)
{
    (void)name; (void)mode;
    mock_fs_open_calls++;
    return mock_fs_next_fd++;
}

int fs_read(int fd, uint8_t *buf, uint32_t n)
{
    (void)fd; (void)buf;
    return (int)n;
}

int fs_write(int fd, const uint8_t *buf, uint32_t n)
{
    (void)fd; (void)buf;
    return (int)n;
}

int fs_close(int fd)
{
    (void)fd;
    mock_fs_close_calls++;
    return 0;
}

int  fs_delete(const char *name)           { (void)name; return 0; }
void fs_list(int (*cb)(const fs_entry_t *)) { (void)cb; }
void fs_format(void)                        {}
