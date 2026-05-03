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

#ifndef KERNEL_FS_H
#define KERNEL_FS_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Filesystem constants
 * ------------------------------------------------------------------------- */
#ifndef FS_MAX_FILES
#define FS_MAX_FILES      32u        /* override via CMake for board-specific sizing */
#endif
#define FS_NAME_MAX       16u
#define FS_BLOCK_SIZE     4096u              /* matches flash erase sector   */
#define FS_FLASH_OFFSET   (1024u * 1024u)   /* 1 MB into flash for FS region */
#ifndef FS_FLASH_SIZE
#define FS_FLASH_SIZE     (512u * 1024u)    /* override via CMake for board-specific sizing */
#endif
#define FS_SUPERBLOCK_MAGIC 0x50494353u     /* "PICS" in little-endian hex   */

/* Maximum file data size: one flash sector (4 KB) per file.
 *
 * Flash layout (each row = one 4 KB sector):
 *   Sector 0           : Superblock (file metadata table)
 *   Sector 1           : File index 0 data
 *   Sector 2           : File index 1 data
 *   ...
 *   Sector FS_MAX_FILES: File index (FS_MAX_FILES-1) data
 *
 * FS_MAX_FILES and FS_FLASH_SIZE are set per board by CMakeLists.txt:
 *   RP2040: FS_MAX_FILES=64,  FS_FLASH_SIZE=1 MB  (2 MB flash - 1 MB firmware)
 *   RP2350: FS_MAX_FILES=127, FS_FLASH_SIZE=3 MB  (4 MB flash - 1 MB firmware)
 * Maximum is 127 files: superblock must fit in one 4 KB sector (12 + N×32 ≤ 4096).
 * RAM cost: superblock cache (~2 KB RP2040 / ~4 KB RP2350) + 4 KB write scratch buffer.
 */
#define FS_MAX_FILE_DATA    FS_BLOCK_SIZE   /* 4 KB max per file             */

/* -------------------------------------------------------------------------
 * Open file-descriptor limit for the filesystem layer.
 * (Separate from the VFS fd table — these are internal FS fds.)
 * ------------------------------------------------------------------------- */
#define FS_MAX_OPEN_FDS   8u

/* -------------------------------------------------------------------------
 * Directory entry (file metadata)
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     used;
    char     name[FS_NAME_MAX];
    uint32_t size;          /* current file size in bytes            */
    uint32_t start_block;   /* index of first data block (0-based)   */
    uint32_t block_count;   /* number of 4 KB blocks allocated       */
} fs_entry_t;

/* -------------------------------------------------------------------------
 * Superblock — first structure in the filesystem region
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t   magic;       /* FS_SUPERBLOCK_MAGIC                   */
    uint32_t   version;     /* filesystem format version             */
    uint32_t   file_count;  /* number of used entries in files[]     */
    fs_entry_t files[FS_MAX_FILES];
} fs_superblock_t;

/* -------------------------------------------------------------------------
 * Filesystem API
 * ------------------------------------------------------------------------- */

/*
 * fs_init — initialise the filesystem.
 *           Reads the superblock from flash; if the magic is valid the
 *           existing filesystem is mounted.  Otherwise a fresh empty
 *           filesystem is created in the flash region.
 */
void fs_init(void);

/*
 * fs_open — open a file by name.
 *   mode uses the same VFS_O_* flags defined in vfs.h.
 *   Returns an internal FS fd on success, or -1 on failure.
 */
int fs_open(const char *name, int mode);

/*
 * fs_read — read up to n bytes from an open FS fd.
 *           Reads directly from XIP-mapped flash (zero RAM overhead).
 */
int fs_read(int fd, uint8_t *buf, uint32_t n);

/*
 * fs_write — write n bytes to an open FS fd.
 *            Data is accumulated in the scratch buffer and committed to
 *            flash when the fd is closed.
 */
int fs_write(int fd, const uint8_t *buf, uint32_t n);

/*
 * fs_close — close an open FS fd.
 *            If the fd was opened for writing and data was written, the
 *            scratch buffer is committed to flash and the superblock is
 *            updated.
 */
int fs_close(int fd);

/*
 * fs_delete — delete a file by name.
 *   Returns 0 on success, -1 if the file is not found.
 */
int fs_delete(const char *name);

/*
 * fs_list — iterate over all files.
 *   The callback is called once per existing file with its fs_entry_t.
 *   Iteration stops early if the callback returns a non-zero value.
 */
void fs_list(int (*callback)(const fs_entry_t *entry));

/*
 * fs_format — erase all files and reset the superblock on flash.
 *             Erases every sector in the FS flash region.
 */
void fs_format(void);

#endif /* KERNEL_FS_H */
