#include "fs.h"
#include "vfs.h"    /* VFS_O_* flags */
#include "arch.h"   /* XIP_BASE, flash/multicore helpers */

#include <stdio.h>

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
 *   Sector 32 (offset +0x020000) : File index 31 data
 *
 * Reading is zero-copy: flash is memory-mapped via XIP so we cast a pointer
 * to the address XIP_BASE + FS_FLASH_OFFSET + sector_offset.
 *
 * Writing requires:
 *   1. Accumulate new data in fs_buffer (RAM).
 *   2. Erase the target sector (sets all bytes to 0xFF).
 *   3. Program the sector from fs_buffer.
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

/* =========================================================================
 * Module state
 * ========================================================================= */

/* In-RAM mirror of the on-flash superblock for fast metadata access. */
static fs_superblock_t superblock_ram;

/* Single shared write-accumulation buffer.
 *
 * Teaching simplification: only one file can be open for writing at a time.
 * This bounds RAM usage to 4 KB regardless of the number of file slots.
 *
 * Data is accumulated here during fs_write() calls and committed to flash
 * (erase + program) when the write-mode fd is closed.
 */
static uint8_t fs_buffer[FS_BLOCK_SIZE];

/* Index of the file currently occupying fs_buffer, or -1 if free. */
static int scratch_owner = -1;   /* file_idx of the writing file, or -1 */

/* Open FS file-descriptor table (internal to fs.c). */
typedef struct {
    bool     used;
    bool     dirty;      /* true: fs_buffer holds uncommitted data      */
    uint32_t file_idx;   /* index into superblock_ram.files[]             */
    uint32_t pos;        /* current read/write position in bytes          */
    int      mode;
} fs_open_fd_t;

static fs_open_fd_t open_fds[FS_MAX_OPEN_FDS];

/* =========================================================================
 * Flash write helpers
 *
 * Every call pairs multicore lockout with interrupt disable so that neither
 * Core 1 nor an ISR fetches from XIP during erase/program.
 *
 * Core 1 must have called multicore_lockout_victim_init() at boot time for
 * multicore_lockout_start_blocking() to return.  See core1_entry() in main.c.
 * ========================================================================= */

/* flash_erase_sector — erase one FS_BLOCK_SIZE (4 KB) flash sector.
 *
 * Ordering is critical:
 *   1. multicore_lockout_start_blocking() halts Core 1 so it cannot fetch
 *      instructions from XIP while the flash bus is in use.
 *   2. save_and_disable_interrupts() prevents any ISR from running on Core 0
 *      (including SysTick / PendSV) between erase and the subsequent program.
 *   3. flash_range_erase() performs the sector erase; all bytes become 0xFF.
 *   4. Interrupts are restored, then Core 1 is released.
 *
 * flash_offset is the byte offset from the START of flash (not the XIP
 * address), and must be 4 KB-aligned. */
static void flash_erase_sector(uint32_t flash_offset)
{
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_offset, FS_BLOCK_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

/* flash_program_sector — program one FS_BLOCK_SIZE (4 KB) sector from src.
 *
 * The sector at flash_offset must already have been erased (all 0xFF) before
 * this call; flash cells can only be programmed from 1 → 0.  Uses the same
 * Core 1 lockout + interrupt-disable sequence as flash_erase_sector.
 * src must remain valid (in RAM) for the duration of the call; passing an
 * XIP-mapped pointer would fault because XIP is suspended during programming. */
static void flash_program_sector(uint32_t flash_offset, const uint8_t *src)
{
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(flash_offset, src, FS_BLOCK_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

/* Write superblock_ram to the superblock flash sector.
 * Uses fs_buffer as the write staging area; call only when fs_buffer is
 * not in use by an open write fd (i.e. after the file data has been committed
 * and scratch_owner has been cleared). */
/* superblock_flush — write superblock_ram to the superblock flash sector.
 *
 * Must only be called when fs_buffer is free (scratch_owner == -1).
 * Procedure:
 *   1. Pre-fill fs_buffer with 0xFF so unused bytes match the erased state
 *      and do not require extra program pulses.
 *   2. Copy superblock_ram into the start of fs_buffer.
 *   3. Erase the superblock sector (SUPERBLOCK_FLASH_OFFSET).
 *   4. Program the sector from fs_buffer.
 *
 * After this call fs_buffer contains the serialised superblock padded with
 * 0xFF.  Callers that need fs_buffer for file data must overwrite it
 * afterwards (e.g. TRUNC zeros it, non-TRUNC copies from XIP). */
static void superblock_flush(void)
{
    /* Flash erase sets all bytes to 0xFF.  Pre-fill with 0xFF so unused
     * bytes in the sector match the erased state. */
    memset(fs_buffer, 0xFF, FS_BLOCK_SIZE);
    memcpy(fs_buffer, &superblock_ram, sizeof(superblock_ram));

    flash_erase_sector(SUPERBLOCK_FLASH_OFFSET);
    flash_program_sector(SUPERBLOCK_FLASH_OFFSET, fs_buffer);
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* find_file — search superblock_ram for a file with the given name.
 * Compares up to FS_NAME_MAX characters (names are NUL-padded, not NUL-
 * terminated beyond the actual length).
 * Returns the file index (0-based) on success, or -1 if not found. */
static int find_file(const char *name)
{
    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (superblock_ram.files[i].used &&
            strncmp(superblock_ram.files[i].name, name, FS_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* alloc_file_entry — find the first unused slot in superblock_ram.files[].
 * Returns the slot index on success, or -1 if the directory is full
 * (FS_MAX_FILES files already exist). */
static int alloc_file_entry(void)
{
    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (!superblock_ram.files[i].used) {
            return (int)i;
        }
    }
    return -1;
}

/* alloc_open_fd — find the first unused slot in the internal open_fds table.
 * Returns the fd index on success, or -1 if all FS_MAX_OPEN_FDS slots are
 * occupied.  The caller must set open_fds[fd].used = true before returning
 * the fd to prevent double-allocation. */
static int alloc_open_fd(void)
{
    for (int i = 0; i < (int)FS_MAX_OPEN_FDS; i++) {
        if (!open_fds[i].used) {
            return i;
        }
    }
    return -1;
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
    memset(open_fds, 0, sizeof(open_fds));
    scratch_owner = -1;

    /* Read the on-flash superblock via XIP. */
    const fs_superblock_t *flash_sb =
        (const fs_superblock_t *)(XIP_BASE + SUPERBLOCK_FLASH_OFFSET);

    if (flash_sb->magic == FS_SUPERBLOCK_MAGIC &&
        flash_sb->version == 1u) {
        /* Valid filesystem found — load the metadata into RAM. */
        memcpy(&superblock_ram, flash_sb, sizeof(superblock_ram));
        printf("[fs] mounted: %u file(s) found\r\n",
               superblock_ram.file_count);
    } else {
        /* No valid filesystem — create a fresh one. */
        printf("[fs] no valid FS found, formatting flash...\r\n");
        fs_format();
    }
}

/* =========================================================================
 * fs_format
 * ========================================================================= */
void fs_format(void)
{
    /* Close all open FDs first. */
    for (int i = 0; i < (int)FS_MAX_OPEN_FDS; i++) {
        open_fds[i].used  = false;
        open_fds[i].dirty = false;
    }
    scratch_owner = -1;

    /* Erase every sector in the FS region (superblock + all file sectors). */
    uint32_t total_sectors = 1u + FS_MAX_FILES;   /* superblock + one per file */
    for (uint32_t s = 0u; s < total_sectors; s++) {
        flash_erase_sector(FS_FLASH_OFFSET + s * FS_BLOCK_SIZE);
    }

    /* Initialise and write a fresh superblock. */
    memset(&superblock_ram, 0, sizeof(superblock_ram));
    superblock_ram.magic      = FS_SUPERBLOCK_MAGIC;
    superblock_ram.version    = 1u;
    superblock_ram.file_count = 0u;

    superblock_flush();

    printf("[fs] format complete\r\n");
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

    int file_idx = find_file(name);

    if (file_idx < 0) {
        /* File does not exist. */
        if (!(mode & VFS_O_CREAT)) {
            return -1;
        }

        /* Create a new entry. */
        file_idx = alloc_file_entry();
        if (file_idx < 0) {
            return -1;   /* directory full */
        }

        fs_entry_t *entry  = &superblock_ram.files[file_idx];
        entry->used        = true;
        entry->size        = 0u;
        entry->start_block = (uint32_t)file_idx;   /* 1:1 index→block mapping */
        entry->block_count = 1u;

        strncpy(entry->name, name, FS_NAME_MAX - 1u);
        entry->name[FS_NAME_MAX - 1u] = '\0';

        superblock_ram.file_count++;

        /* Persist the new superblock entry immediately so that even if the
         * caller never writes any data, the file shows up after a reboot. */
        superblock_flush();
    }

    /* If writing: claim the scratch buffer.
     * VFS_O_WRONLY (0x02) is bit 1; VFS_O_RDWR (0x03) also has bit 1 set.
     * VFS_O_RDONLY (0x01) does NOT have bit 1, so this correctly excludes
     * read-only opens.  Using (mode & VFS_O_RDWR) would be wrong because
     * VFS_O_RDONLY shares bit 0 with VFS_O_RDWR (0x01 & 0x03 = 0x01 != 0). */
    bool writing = (mode & VFS_O_WRONLY) != 0;
    if (writing) {
        if (scratch_owner != -1) {
            /* Another file is already using the write buffer. */
            return -1;
        }
        scratch_owner = file_idx;

        if (mode & VFS_O_TRUNC) {
            /* Truncate: start with a blank buffer. */
            memset(fs_buffer, 0, FS_BLOCK_SIZE);
            superblock_ram.files[file_idx].size = 0u;
        } else {
            /* Preserve existing content so append/overwrite works. */
            memcpy(fs_buffer,
                   (const uint8_t *)(XIP_BASE + FILE_FLASH_OFFSET(file_idx)),
                   FS_BLOCK_SIZE);
        }
    }

    int fd = alloc_open_fd();
    if (fd < 0) {
        if (writing) {
            scratch_owner = -1;
        }
        return -1;
    }

    open_fds[fd].used     = true;
    open_fds[fd].dirty    = false;
    open_fds[fd].file_idx = (uint32_t)file_idx;
    open_fds[fd].pos      = 0u;
    open_fds[fd].mode     = mode;

    return fd;
}

/* =========================================================================
 * fs_read
 *
 * For files that are not being written: read directly from XIP-mapped flash
 * (no RAM copy, zero overhead).
 *
 * For the file currently in the scratch buffer (opened for write and dirty):
 * read from fs_buffer so that in-progress writes are visible.
 * ========================================================================= */
int fs_read(int fd, uint8_t *buf, uint32_t n)
{
    if (fd < 0 || fd >= (int)FS_MAX_OPEN_FDS || !open_fds[fd].used) {
        return -1;
    }
    if (buf == NULL || n == 0u) {
        return 0;
    }

    fs_open_fd_t *ofd   = &open_fds[fd];
    fs_entry_t   *entry = &superblock_ram.files[ofd->file_idx];

    uint32_t available = entry->size > ofd->pos ? entry->size - ofd->pos : 0u;
    uint32_t to_read   = n < available ? n : available;

    if (to_read == 0u) {
        return 0;   /* EOF */
    }

    if ((int)ofd->file_idx == scratch_owner && ofd->dirty) {
        /* File is being written — read from fs_buffer. */
        memcpy(buf, fs_buffer + ofd->pos, to_read);
    } else {
        /* Read directly from XIP flash — no RAM copy required. */
        memcpy(buf, FILE_XIP_PTR(ofd->file_idx, ofd->pos), to_read);
    }

    ofd->pos += to_read;
    return (int)to_read;
}

/* =========================================================================
 * fs_write
 *
 * Accumulates data in fs_buffer.  Actual flash erase+program happens in
 * fs_close() so that multiple fs_write() calls on the same fd are efficient
 * (one flash operation per close, not per write).
 * ========================================================================= */
int fs_write(int fd, const uint8_t *buf, uint32_t n)
{
    if (fd < 0 || fd >= (int)FS_MAX_OPEN_FDS || !open_fds[fd].used) {
        return -1;
    }
    if (buf == NULL || n == 0u) {
        return 0;
    }

    fs_open_fd_t *ofd   = &open_fds[fd];
    fs_entry_t   *entry = &superblock_ram.files[ofd->file_idx];

    /* Only write-mode fds can write. */
    if (!(ofd->mode & VFS_O_WRONLY)) {
        return -1;
    }

    /* Clamp to the capacity of one flash sector. */
    uint32_t capacity  = FS_MAX_FILE_DATA;
    uint32_t remaining = capacity > ofd->pos ? capacity - ofd->pos : 0u;
    uint32_t to_write  = n < remaining ? n : remaining;

    if (to_write == 0u) {
        return -1;   /* no space */
    }

    memcpy(fs_buffer + ofd->pos, buf, to_write);
    ofd->pos += to_write;
    ofd->dirty = true;

    if (ofd->pos > entry->size) {
        entry->size = ofd->pos;
    }

    return (int)to_write;
}

/* =========================================================================
 * fs_close
 *
 * If the fd has pending writes (dirty), commit fs_buffer to flash:
 *   1. Erase the file's data sector.
 *   2. Program the sector with fs_buffer contents.
 *   3. Flush the updated superblock (file size may have changed).
 * ========================================================================= */
int fs_close(int fd)
{
    if (fd < 0 || fd >= (int)FS_MAX_OPEN_FDS || !open_fds[fd].used) {
        return -1;
    }

    fs_open_fd_t *ofd = &open_fds[fd];

    if (ofd->dirty) {
        uint32_t file_idx    = ofd->file_idx;
        uint32_t flash_offset = FILE_FLASH_OFFSET(file_idx);

        /* Step 1 & 2: erase the data sector then program from fs_buffer. */
        flash_erase_sector(flash_offset);
        flash_program_sector(flash_offset, fs_buffer);

        /* Step 3: fs_buffer is now free — use it to flush the superblock. */
        scratch_owner = -1;
        superblock_flush();
    }

    open_fds[fd].used  = false;
    open_fds[fd].dirty = false;
    return 0;
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

    int idx = find_file(name);
    if (idx < 0) {
        return -1;
    }

    /* Force-close any open FDs pointing to this file. */
    for (int i = 0; i < (int)FS_MAX_OPEN_FDS; i++) {
        if (open_fds[i].used && open_fds[i].file_idx == (uint32_t)idx) {
            open_fds[i].used  = false;
            open_fds[i].dirty = false;
        }
    }
    if (scratch_owner == idx) {
        scratch_owner = -1;
    }

    /* Remove the metadata entry. */
    memset(&superblock_ram.files[idx], 0, sizeof(fs_entry_t));
    if (superblock_ram.file_count > 0u) {
        superblock_ram.file_count--;
    }

    /* Erase the file's data sector so the storage is genuinely freed. */
    flash_erase_sector(FILE_FLASH_OFFSET((uint32_t)idx));

    /* Persist the updated superblock. */
    superblock_flush();

    return 0;
}

/* =========================================================================
 * fs_list
 * ========================================================================= */
void fs_list(int (*callback)(const fs_entry_t *entry))
{
    if (callback == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < FS_MAX_FILES; i++) {
        if (superblock_ram.files[i].used) {
            int rc = callback(&superblock_ram.files[i]);
            if (rc != 0) {
                break;
            }
        }
    }
}
