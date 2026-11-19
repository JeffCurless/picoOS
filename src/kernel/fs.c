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

#include "fs.h"
#include "vfs.h"    /* VFS_O_* flags */
#include "arch.h"   /* XIP_BASE, flash_safe_execute, multicore helpers */
#include "sync.h"   /* kmutex_t */
#include "sched.h"  /* sched_yield — move to core 0 for flash writes */

#include <stdio.h>

#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Flash layout
 *
 * The filesystem occupies a contiguous region of the RP2040's external QSPI
 * flash starting at FS_FLASH_OFFSET (1 MB from the start of flash).
 *
 *   Sector 0  (offset +0x000000) : Superblock — fs_superblock_t metadata
 *   Sector 1  (offset +0x001000) : File index 0 data (up to 4 KB)
 *   Sector 2  (offset +0x002000) : File index 1 data
 *   ...
 *   Sector N  (N = FS_MAX_FILES) : File index N-1 data
 *
 * Reading is zero-copy: flash is memory-mapped via XIP so we cast a pointer
 * to the address XIP_BASE + FS_FLASH_OFFSET + sector_offset.
 *
 * Writing requires:
 *   1. Accumulate new data in fs_buffer (RAM).
 *   2. Erase the target sector (sets all bytes to 0xFF).
 *   3. Program the pages of the sector that hold data, from fs_buffer.
 *
 * CRITICAL: During any flash_range_erase / flash_range_program call the
 * CPU must not fetch instructions from XIP.  Core 1 is paused via the
 * multicore lockout mechanism before every flash operation.
 * ========================================================================= */

/* Flash offset of the superblock sector. */
#define SUPERBLOCK_FLASH_OFFSET  FS_FLASH_OFFSET

/* Flash offset of file n's data sector (sectors are 1-indexed after the
 * superblock sector). */
#define FILE_FLASH_OFFSET(n)  \
    (FS_FLASH_OFFSET + ((uint32_t)(n) + 1u) * FS_BLOCK_SIZE)

/* XIP read pointer into file n's data at byte offset off. */
#define FILE_XIP_PTR(n, off) \
    ((const uint8_t *)(XIP_BASE + FILE_FLASH_OFFSET(n) + (uint32_t)(off)))

/* Version 1 of the superblock: the same, minus max_files.  Read only, to
 * convert an old filesystem in place (see mount()). */
typedef struct {
    uint32_t   magic;
    uint32_t   version;
    uint32_t   file_count;
    fs_entry_t files[];
} fs_superblock_v1_t;

/* =========================================================================
 * Module state
 * ========================================================================= */

/* Mutex protecting all shared filesystem state: open_fds[], superblock_ram,
 * scratch_owner, scratch_size and fs_buffer.  Initialized in fs_init().
 *
 * fs_take_lock / fs_give_lock skip locking before the scheduler is running
 * (CURRENT_TCB == NULL) so that fs_init() and fs_format() can be called from
 * main() before sched_start() without accessing scheduler state. */
static kmutex_t fs_mutex;

static inline void fs_take_lock(void)
{
    if (CURRENT_TCB != NULL) { kmutex_lock(&fs_mutex); }
}

static inline void fs_give_lock(void)
{
    if (CURRENT_TCB != NULL) { kmutex_unlock(&fs_mutex); }
}

/* In-RAM mirror of the on-flash superblock for fast metadata access.
 *
 * It holds the committed state only: a file's size changes here when
 * fs_close() has written its data, never while a write is pending.  So the
 * mirror can be flushed at any time (another file created or deleted) and
 * always describes what is on flash.
 *
 * flash_range_program() writes whole 256-byte pages from RAM, so the mirror
 * is padded to a page multiple and programmed straight from here.  It never
 * goes through fs_buffer, which may hold another file's unsaved data. */
#define SUPERBLOCK_PROG_SIZE \
    ((sizeof(fs_superblock_t) + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

_Static_assert(SUPERBLOCK_PROG_SIZE <= FS_BLOCK_SIZE,
               "superblock must fit in one sector (FS_MAX_FILES too large)");

static union {
    fs_superblock_t sb;
    uint8_t         pages[SUPERBLOCK_PROG_SIZE];   /* tail is don't-care */
} superblock_store;

static fs_superblock_t *const superblock_ram = &superblock_store.sb;

/* Single shared write-accumulation buffer.
 *
 * Teaching simplification: only one file can be open for writing at a time.
 * This bounds RAM usage to 4 KB regardless of the number of file slots.
 *
 * The writer's fd works on this copy of the file; fs_close() commits it to
 * flash (erase + program).  scratch_size is the size the file will have
 * then.  Until the commit, every other fd sees the file as it is on flash.
 */
static uint8_t  fs_buffer[FS_BLOCK_SIZE];
static int      scratch_owner = -1;   /* file_idx of the writing file, or -1 */
static uint32_t scratch_size  = 0u;   /* bytes of data in fs_buffer          */

/* Open FS file-descriptor table (internal to fs.c).
 *
 * A file deleted (or formatted away) while open leaves its fds allocated
 * but stale, rather than freeing them: the VFS still holds the fd number,
 * and a freed slot could be handed to another file, which that VFS fd would
 * then read and write.  A stale fd fails every read and write; fs_close()
 * frees it. */
typedef struct {
    bool     used;
    bool     stale;      /* its file is gone: I/O fails, close frees it   */
    bool     writer;     /* holds fs_buffer (opened with VFS_O_WRONLY)    */
    bool     dirty;      /* writer only: fs_buffer differs from flash     */
    uint32_t file_idx;   /* index into superblock_ram->files[]            */
    uint32_t pos;        /* current read/write position in bytes          */
} fs_open_fd_t;

static fs_open_fd_t open_fds[FS_MAX_OPEN_FDS];

/* =========================================================================
 * Flash write helpers — using flash_safe_execute() (pico_flash library)
 *
 * flash_safe_execute() is the SDK-provided thread-safe wrapper for flash
 * erase/program operations.  Internally it:
 *   1. Pauses Core 1 via multicore_lockout_start_blocking() (Core 1 must
 *      have called multicore_lockout_victim_init() at boot — see main.c).
 *   2. Disables interrupts on the calling core.
 *   3. Invokes the callback (which calls flash_range_erase/program).
 *   4. Restores interrupts, then releases Core 1.
 *
 * Using the SDK API rather than the manual lockout sequence ensures picoOS
 * stays compatible as the SDK evolves and centralises the safety protocol
 * in one well-tested place.
 *
 * UINT32_MAX as the timeout gives effectively "wait forever" behaviour,
 * matching the original multicore_lockout_start_blocking() semantics.
 *
 * Core 0 only: multicore lockout is one-way.  flash_safe_execute() can only
 * pause the *other* core if that core called multicore_lockout_victim_init(),
 * and only Core 1 does (main.c).  Called from Core 1 it returns
 * PICO_ERROR_NOT_PERMITTED without running the callback.  The scheduler may
 * run the calling thread on either core, and may move it between two flash
 * operations, so fs_flash_safe() pins the thread to Core 0 for the call.
 * ========================================================================= */

typedef struct { uint32_t offset; }                         fs_erase_cb_t;
typedef struct { uint32_t offset; const uint8_t *src; uint32_t len; } fs_program_cb_t;

static void do_flash_erase(void *param)
{
    const fs_erase_cb_t *a = (const fs_erase_cb_t *)param;
    flash_range_erase(a->offset, FS_BLOCK_SIZE);
}

static void do_flash_program(void *param)
{
    const fs_program_cb_t *a = (const fs_program_cb_t *)param;
    flash_range_program(a->offset, a->src, a->len);
}

/* Run fn(param) through flash_safe_execute() on Core 0.  Returns PICO_OK
 * (0) or the SDK error code.  Before the scheduler starts (fs_init from
 * main) there is no current thread and main already runs on Core 0. */
static int fs_flash_safe(void (*fn)(void *), void *param)
{
    tcb_t  *self  = CURRENT_TCB;
    int8_t  saved = THREAD_AFFINITY_ANY;

    if (self != NULL) {
        saved = self->affinity;
        self->affinity = THREAD_AFFINITY_C0;
        while (get_core_num() != 0u) {
            sched_yield();          /* Core 0 picks us up at its next switch */
        }
    }

    int rc = flash_safe_execute(fn, param, UINT32_MAX);

    if (self != NULL) {
        self->affinity = saved;
    }
    if (rc != 0) {
        printf("[fs] flash operation failed (rc %d)\r\n", rc);
    }
    return rc;
}

static int flash_erase_sector(uint32_t flash_offset)
{
    fs_erase_cb_t args = { flash_offset };
    return fs_flash_safe(do_flash_erase, &args);
}

/* flash_program — program the first len bytes of src, rounded up to whole
 * pages (the unit flash_range_program() needs).  The sector must have been
 * erased.  A 10-byte file costs one 256-byte page, not the whole 4 KB
 * sector; an empty one costs nothing. */
static int flash_program(uint32_t flash_offset, const uint8_t *src, uint32_t len)
{
    len = (len + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    if (len == 0u) {
        return 0;
    }
    fs_program_cb_t args = { flash_offset, src, len };
    return fs_flash_safe(do_flash_program, &args);
}

/* superblock_flush — write superblock_ram to the superblock flash sector:
 * erase the sector, then program the page-padded mirror directly.  Safe at
 * any time — it does not touch fs_buffer, so a file open for writing keeps
 * its unsaved data while another file is created or deleted. */
static int superblock_flush(void)
{
    if (flash_erase_sector(SUPERBLOCK_FLASH_OFFSET) != 0) return -1;
    return flash_program(SUPERBLOCK_FLASH_OFFSET, superblock_store.pages,
                         SUPERBLOCK_PROG_SIZE) != 0 ? -1 : 0;
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* name_valid — a name must fit in fs_entry_t.name with its NUL: 1 to
 * FS_NAME_MAX - 1 characters.  A longer name used to be stored cut short,
 * which find_file() then never matched: every create made another
 * duplicate, and rm with the full name could not delete any of them. */
static bool name_valid(const char *name)
{
    return name[0] != '\0' && memchr(name, '\0', FS_NAME_MAX) != NULL;
}

/* find_file — search superblock_ram for a file with the given name.
 * Compares up to FS_NAME_MAX characters (names are NUL-padded, not NUL-
 * terminated beyond the actual length).
 * Returns the file index (0-based) on success, or -1 if not found. */
static int find_file(const char *name)
{
    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (superblock_ram->files[i].used &&
            strncmp(superblock_ram->files[i].name, name, FS_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* alloc_file_entry — find the first unused slot in superblock_ram->files[].
 * Returns the slot index on success, or -1 if the directory is full
 * (FS_MAX_FILES files already exist). */
static int alloc_file_entry(void)
{
    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (!superblock_ram->files[i].used) {
            return (int)i;
        }
    }
    return -1;
}

/* alloc_open_fd — find the first unused slot in the internal open_fds table.
 * Returns the fd index on success, or -1 if all FS_MAX_OPEN_FDS slots are
 * occupied.  The caller must set open_fds[fd].used = true before releasing
 * fs_mutex to prevent double-allocation. */
static int alloc_open_fd(void)
{
    for (int i = 0; i < (int)FS_MAX_OPEN_FDS; i++) {
        if (!open_fds[i].used) {
            return i;
        }
    }
    return -1;
}

/* fd_get — the open fd for an fd number, or NULL.  Caller holds fs_mutex:
 * checked outside it, the slot could be closed (or reused) in between. */
static fs_open_fd_t *fd_get(int fd)
{
    if (fd < 0 || fd >= (int)FS_MAX_OPEN_FDS || !open_fds[fd].used) {
        return NULL;
    }
    return &open_fds[fd];
}

/* fds_go_stale — the file at file_idx is going away (idx < 0: every file).
 * Its fds stay allocated but stale, and a pending write to it is dropped.
 * Caller holds fs_mutex. */
static void fds_go_stale(int file_idx)
{
    for (int i = 0; i < (int)FS_MAX_OPEN_FDS; i++) {
        fs_open_fd_t *f = &open_fds[i];
        if (f->used && (file_idx < 0 || f->file_idx == (uint32_t)file_idx)) {
            f->stale  = true;
            f->writer = false;
            f->dirty  = false;
        }
    }
    if (file_idx < 0 || scratch_owner == file_idx) {
        scratch_owner = -1;
        scratch_size  = 0u;
    }
}

/* entry_valid — is e (table slot idx) an entry as fs_open() writes it?
 * The erased (0xFF) tail of a table written by a build with fewer
 * FS_MAX_FILES, or a corrupt entry, is not. */
static bool entry_valid(const fs_entry_t *e, uint32_t idx)
{
    uint8_t used;
    memcpy(&used, &e->used, 1u);   /* raw byte: erased flash is 0xFF, not a bool */
    return used == 1u && name_valid(e->name) &&
           e->start_block == idx && e->block_count == 1u &&
           e->size <= FS_MAX_FILE_DATA;
}

/* mount — load the on-flash superblock into superblock_ram.  Returns false
 * (superblock_ram untouched) if there is no filesystem to mount.
 *
 * File i always lives in sector i + 1, whatever the table size, so a table
 * from another layout converts entry by entry:
 *   version 1         no max_files field; its table length is unknown, so
 *                     the entries are checked and invalid ones dropped
 *   other max_files   entries past this build's FS_MAX_FILES are dropped
 * Either way the table is written back as version 2 with this build's
 * FS_MAX_FILES, so the next boot mounts it as it is. */
static bool mount(void)
{
    const fs_superblock_t *sb =
        (const fs_superblock_t *)(XIP_BASE + SUPERBLOCK_FLASH_OFFSET);
    const fs_entry_t *files;
    uint32_t          n;

    if (sb->magic != FS_SUPERBLOCK_MAGIC) {
        return false;
    }
    if (sb->version == FS_VERSION) {
        files = sb->files;
        n     = sb->max_files;
    } else if (sb->version == 1u) {
        files = ((const fs_superblock_v1_t *)sb)->files;
        n     = FS_MAX_FILES;
    } else {
        return false;
    }

    uint32_t stored  = sb->file_count;
    bool     convert = sb->version != FS_VERSION || n != FS_MAX_FILES;
    uint32_t keep    = (n < FS_MAX_FILES) ? n : FS_MAX_FILES;

    memset(superblock_ram, 0, sizeof(*superblock_ram));
    superblock_ram->magic     = FS_SUPERBLOCK_MAGIC;
    superblock_ram->version   = FS_VERSION;
    superblock_ram->max_files = FS_MAX_FILES;
    for (uint32_t i = 0u; i < keep; i++) {
        if (entry_valid(&files[i], i)) {
            superblock_ram->files[i] = files[i];
            superblock_ram->file_count++;
        }
    }

    if (convert || superblock_ram->file_count != stored) {
        printf("[fs] converting table (v%u, %u slots) to v%u, %u slots: "
               "%u of %u file(s) kept\r\n",
               (unsigned)sb->version, (unsigned)n, (unsigned)FS_VERSION,
               (unsigned)FS_MAX_FILES, (unsigned)superblock_ram->file_count,
               (unsigned)stored);
        superblock_flush();   /* erases the sector sb points into: last use */
    }
    return true;
}

/* =========================================================================
 * fs_init
 *
 * Try to mount an existing filesystem from flash.  If the superblock magic
 * is present the metadata is loaded; files can then be read directly from
 * the XIP-mapped flash data sectors.  If the magic is absent (first boot
 * or after a flash erase) a fresh empty filesystem is formatted.
 * ========================================================================= */
void fs_init(void)
{
    kmutex_init(&fs_mutex);   /* must be first; subsequent ops may acquire it */
    memset(open_fds, 0, sizeof(open_fds));
    scratch_owner = -1;
    scratch_size  = 0u;

    if (mount()) {
        printf("[fs] mounted: %u file(s) found\r\n",
               (unsigned)superblock_ram->file_count);
    } else {
        /* No valid filesystem — create a fresh one. */
        printf("[fs] no valid FS found, formatting flash...\r\n");
        fs_format();
    }
}

/* =========================================================================
 * fs_format
 *
 * Erases the superblock and the data sector of each file that exists.  The
 * free sectors are left as they are: fs_close() erases a sector before it
 * programs it, and nothing reads a file past its size, so old bytes there
 * are never seen.  That is one interrupts-off flash operation per file
 * instead of one per sector of the whole region (65 on RP2040, 128 on
 * RP2350).  On first boot superblock_ram is all zeros, so only the
 * superblock is erased.
 * ========================================================================= */
void fs_format(void)
{
    fs_take_lock();

    fds_go_stale(-1);

    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (superblock_ram->files[i].used) {
            flash_erase_sector(FILE_FLASH_OFFSET(i));
        }
    }

    /* Initialise and write a fresh superblock. */
    memset(superblock_ram, 0, sizeof(*superblock_ram));
    superblock_ram->magic     = FS_SUPERBLOCK_MAGIC;
    superblock_ram->version   = FS_VERSION;
    superblock_ram->max_files = FS_MAX_FILES;

    superblock_flush();

    printf("[fs] format complete\r\n");
    fs_give_lock();
}

/* =========================================================================
 * fs_open
 * ========================================================================= */
int fs_open(const char *name, int mode)
{
    if (name == NULL) {
        return -1;
    }

    /* Strip any leading '/' for comparison. */
    if (name[0] == '/') {
        name++;
    }
    if (!name_valid(name)) {
        return -1;
    }

    /* VFS_O_WRONLY (0x02) is bit 1; VFS_O_RDWR (0x03) also has bit 1 set.
     * VFS_O_RDONLY (0x01) does NOT have bit 1, so this correctly excludes
     * read-only opens.  Using (mode & VFS_O_RDWR) would be wrong because
     * VFS_O_RDONLY shares bit 0 with VFS_O_RDWR (0x01 & 0x03 = 0x01 != 0). */
    bool writing = (mode & VFS_O_WRONLY) != 0;

    fs_take_lock();

    /* Check everything that can fail before creating the file, so a failed
     * open leaves no new entry behind. */
    int fd = alloc_open_fd();
    if (fd < 0 || (writing && scratch_owner != -1)) {
        fd = -1;   /* no fd free, or another file holds the write buffer */
        goto out;
    }

    int file_idx = find_file(name);
    if (file_idx < 0) {
        /* File does not exist. */
        if (!(mode & VFS_O_CREAT)) {
            fd = -1;
            goto out;
        }

        /* Create a new entry. */
        file_idx = alloc_file_entry();
        if (file_idx < 0) {
            fd = -1;
            goto out;   /* directory full */
        }

        fs_entry_t *entry  = &superblock_ram->files[file_idx];
        entry->used        = true;
        entry->size        = 0u;
        entry->start_block = (uint32_t)file_idx;   /* 1:1 index→block mapping */
        entry->block_count = 1u;

        strncpy(entry->name, name, FS_NAME_MAX - 1u);
        entry->name[FS_NAME_MAX - 1u] = '\0';

        superblock_ram->file_count++;

        /* Persist the new superblock entry immediately so that even if the
         * caller never writes any data, the file shows up after a reboot. */
        superblock_flush();
    }

    fs_open_fd_t *ofd = &open_fds[fd];
    memset(ofd, 0, sizeof(*ofd));
    ofd->used     = true;
    ofd->writer   = writing;
    ofd->file_idx = (uint32_t)file_idx;

    if (writing) {
        uint32_t size = superblock_ram->files[file_idx].size;
        scratch_owner = file_idx;
        if (mode & VFS_O_TRUNC) {
            /* Start from an empty file.  Dirty if that is a change, so
             * fs_close() commits it: setting only the size here would show
             * 0 bytes until the next reboot brought the old data back. */
            memset(fs_buffer, 0xFF, FS_BLOCK_SIZE);
            scratch_size = 0u;
            ofd->dirty   = (size != 0u);
        } else {
            /* Preserve existing content so append/overwrite works. */
            memcpy(fs_buffer, FILE_XIP_PTR(file_idx, 0u), FS_BLOCK_SIZE);
            scratch_size = size;
        }
        /* When appending, start the write cursor at the current end of
         * file so new data is placed after existing content. */
        if (mode & VFS_O_APPEND) {
            ofd->pos = scratch_size;
        }
    }

out:
    fs_give_lock();
    return fd;
}

/* =========================================================================
 * fs_read
 *
 * The writer's fd reads its own pending copy in fs_buffer; any other fd
 * reads flash, up to the committed size, so it never sees half a write.
 * ========================================================================= */
int fs_read(int fd, uint8_t *buf, uint32_t n)
{
    fs_take_lock();

    fs_open_fd_t *ofd    = fd_get(fd);
    int           result = 0;

    if (ofd == NULL || ofd->stale) {
        result = -1;
    } else if (buf != NULL && n > 0u) {
        uint32_t size      = ofd->writer ? scratch_size
                                         : superblock_ram->files[ofd->file_idx].size;
        uint32_t available = size > ofd->pos ? size - ofd->pos : 0u;
        uint32_t to_read   = n < available ? n : available;

        if (to_read > 0u) {
            const uint8_t *src = ofd->writer ? fs_buffer + ofd->pos
                                             : FILE_XIP_PTR(ofd->file_idx, ofd->pos);
            memcpy(buf, src, to_read);
            ofd->pos += to_read;
            result = (int)to_read;
        }
    }

    fs_give_lock();
    return result;
}

/* =========================================================================
 * fs_write
 *
 * Data goes into fs_buffer only; flash is untouched until fs_close().
 * Batching all writes into a single erase+program cycle at close time
 * keeps multiple fs_write() calls on the same fd cheap.
 * ========================================================================= */
int fs_write(int fd, const uint8_t *buf, uint32_t n)
{
    fs_take_lock();

    fs_open_fd_t *ofd    = fd_get(fd);
    int           result = -1;

    /* Only the writer's fd can write; a stale fd never can. */
    if (ofd != NULL && ofd->writer) {
        if (buf == NULL || n == 0u) {
            result = 0;
        } else {
            /* Clamp to the capacity of one flash sector. */
            uint32_t remaining = FS_MAX_FILE_DATA > ofd->pos ? FS_MAX_FILE_DATA - ofd->pos : 0u;
            uint32_t to_write  = n < remaining ? n : remaining;

            if (to_write > 0u) {
                memcpy(fs_buffer + ofd->pos, buf, to_write);
                ofd->pos  += to_write;
                ofd->dirty = true;
                if (ofd->pos > scratch_size) {
                    scratch_size = ofd->pos;
                }
                result = (int)to_write;
            }
        }
    }

    fs_give_lock();
    return result;
}

/* =========================================================================
 * fs_close
 *
 * For the writer's fd, if the file changed:
 *   1. Erase the file's flash sector.
 *   2. Program the pages of fs_buffer that hold data.
 *   3. Publish the new size, and flush the superblock if it changed.
 * The write buffer is released either way.  A stale fd is just freed.
 *
 * If step 1 or 2 fails the old size stays published, but the old data may
 * already be erased: there is no crash recovery yet (docs/imperfections.md).
 * ========================================================================= */
int fs_close(int fd)
{
    fs_take_lock();

    fs_open_fd_t *ofd = fd_get(fd);
    if (ofd == NULL) {
        fs_give_lock();
        return -1;
    }

    int rc = 0;
    if (ofd->writer) {
        if (ofd->dirty) {
            fs_entry_t *entry        = &superblock_ram->files[ofd->file_idx];
            uint32_t    flash_offset = FILE_FLASH_OFFSET(ofd->file_idx);

            if (flash_erase_sector(flash_offset) != 0 ||
                flash_program(flash_offset, fs_buffer, scratch_size) != 0) {
                rc = -1;
            } else if (entry->size != scratch_size) {
                entry->size = scratch_size;
                if (superblock_flush() != 0) {
                    rc = -1;
                }
            }
        }
        scratch_owner = -1;
        scratch_size  = 0u;
    }

    memset(ofd, 0, sizeof(*ofd));   /* used = false */

    fs_give_lock();
    return rc;
}

/* =========================================================================
 * fs_delete
 * ========================================================================= */
int fs_delete(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    if (name[0] == '/') {
        name++;
    }
    if (!name_valid(name)) {
        return -1;   /* no such name can be stored, so it is not there */
    }

    fs_take_lock();

    int idx = find_file(name);
    if (idx < 0) {
        fs_give_lock();
        return -1;
    }

    /* Open fds on it go stale (see fs_open_fd_t); a pending write is lost. */
    fds_go_stale(idx);

    /* Remove the metadata entry. */
    memset(&superblock_ram->files[idx], 0, sizeof(fs_entry_t));
    if (superblock_ram->file_count > 0u) {
        superblock_ram->file_count--;
    }

    /* Erase the file's data sector so the storage is genuinely freed, then
     * persist the updated superblock. */
    int rc = 0;
    if (flash_erase_sector(FILE_FLASH_OFFSET((uint32_t)idx)) != 0 ||
        superblock_flush() != 0) {
        rc = -1;
    }

    fs_give_lock();
    return rc;
}

/* =========================================================================
 * fs_list
 *
 * Sizes are the committed ones: a file being written shows its old size
 * until fs_close() commits the new data.
 * ========================================================================= */
void fs_list(int (*callback)(const fs_entry_t *entry))
{
    if (callback == NULL) {
        return;
    }

    fs_take_lock();

    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (superblock_ram->files[i].used) {
            int rc = callback(&superblock_ram->files[i]);
            if (rc != 0) {
                break;
            }
        }
    }

    fs_give_lock();
}
