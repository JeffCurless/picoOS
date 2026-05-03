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
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>
#include <stdbool.h>
#include "dev.h"

/* -------------------------------------------------------------------------
 * VFS constants
 * ------------------------------------------------------------------------- */
#define VFS_MAX_OPEN  16u
#define VFS_PATH_MAX  64u

/* Open-mode flags (may be OR'd together). */
#define VFS_O_RDONLY  0x01
#define VFS_O_WRONLY  0x02
#define VFS_O_RDWR    0x03
#define VFS_O_CREAT   0x04
#define VFS_O_TRUNC   0x08
#define VFS_O_APPEND  0x10  /* start write position at end of file */

/* -------------------------------------------------------------------------
 * File types
 * ------------------------------------------------------------------------- */
typedef enum {
    VFS_TYPE_FILE,   /* backed by the RAM/flash filesystem   */
    VFS_TYPE_DEV     /* backed by a device_t in the dev table */
} vfs_type_t;

/* -------------------------------------------------------------------------
 * Open file descriptor entry
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     used;
    vfs_type_t type;
    char     path[VFS_PATH_MAX];
    uint32_t pos;
    dev_id_t dev_id;       /* valid when type == VFS_TYPE_DEV  */
    uint32_t fs_file_id;   /* valid when type == VFS_TYPE_FILE */
    int      mode;
} vfs_fd_t;

/* -------------------------------------------------------------------------
 * VFS API
 * ------------------------------------------------------------------------- */

void vfs_init(void);

/*
 * vfs_open — open a file or device by path.
 *   Returns a non-negative fd on success, or -1 on failure.
 *   Device paths: /dev/console, /dev/timer, /dev/flash, /dev/gpio.
 *   All other paths are forwarded to the filesystem layer.
 */
int vfs_open(const char *path, int mode);

/*
 * vfs_read — read up to n bytes from fd into buf.
 *   Returns the number of bytes read, or -1 on error.
 */
int vfs_read(int fd, uint8_t *buf, uint32_t n);

/*
 * vfs_write — write n bytes from buf to fd.
 *   Returns the number of bytes written, or -1 on error.
 */
int vfs_write(int fd, const uint8_t *buf, uint32_t n);

/*
 * vfs_close — close an open file descriptor.
 *   Returns 0 on success, or -1 on error.
 */
int vfs_close(int fd);

/*
 * vfs_mount_dev — register a device so it can be opened by path.
 *   e.g. vfs_mount_dev("/dev/console", DEV_CONSOLE)
 */
int vfs_mount_dev(const char *path, dev_id_t id);

#endif /* KERNEL_VFS_H */
