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
 * tests/fs/test_fs.c — host-native tests for the picoOS flash filesystem.
 *
 * Flash hardware (XIP reads, erase, program, multicore lockout) is replaced
 * by the RAM-backed mock in mock_flash.c.  Each test calls fs_reset() which
 * re-initialises the mock buffer and formats a fresh filesystem.
 *
 * Build (from tests/):
 *   cmake -B build && make -C build
 *   ./build/test_fs
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "fs.h"
#include "arch.h"   /* FLASH_PAGE_SIZE (host stubs) */
#include "vfs.h"    /* VFS_O_* flags */
#include "mock_flash.h"
#include "../framework.h"

/* -------------------------------------------------------------------------
 * Per-test setup: reset mock flash and format a clean filesystem.
 * ------------------------------------------------------------------------- */
static void fs_reset(void)
{
    mock_flash_init();
    fs_init();     /* initialises fs_mutex; formats the blank (0xFF) flash */
    fs_format();
}

/* -------------------------------------------------------------------------
 * Helper: count files via fs_list()
 * ------------------------------------------------------------------------- */
static int file_count;
static int count_files_cb(const fs_entry_t *e) { (void)e; file_count++; return 0; }

static int count_files(void)
{
    file_count = 0;
    fs_list(count_files_cb);
    return file_count;
}

/* -------------------------------------------------------------------------
 * Helper: find a file's size via fs_list()
 * ------------------------------------------------------------------------- */
static const char *find_name;
static uint32_t   found_size;
static int find_file_cb(const fs_entry_t *e)
{
    if (strncmp(e->name, find_name, FS_NAME_MAX) == 0) {
        found_size = e->size;
        return 1;   /* stop iteration */
    }
    return 0;
}

static int file_size(const char *name)
{
    find_name  = name;
    found_size = (uint32_t)-1;
    fs_list(find_file_cb);
    return (found_size == (uint32_t)-1) ? -1 : (int)found_size;
}

/* -------------------------------------------------------------------------
 * test_format_creates_valid_superblock
 * After fs_format(), the on-flash superblock must have the correct magic,
 * version, and a file_count of zero.
 * ------------------------------------------------------------------------- */
static void test_format_creates_valid_superblock(void)
{
    BEGIN_TEST(format_creates_valid_superblock);
    mock_flash_init();
    fs_init();     /* initialises fs_mutex before fs_format() takes it */
    fs_format();

    /* Re-read the superblock through fs_init to confirm flash contents. */
    fs_init();
    CHECK(count_files() == 0, "file_count must be 0 after fresh format");

    /* Verify the raw superblock in mock flash (at offset 0 = superblock sector). */
    const uint32_t *raw = (const uint32_t *)mock_flash_buf;
    CHECK(raw[0] == FS_SUPERBLOCK_MAGIC, "magic must be FS_SUPERBLOCK_MAGIC");
    CHECK(raw[1] == FS_VERSION,          "version must be FS_VERSION (2)");
    CHECK(raw[2] == 0u,                  "file_count must be 0 in on-flash superblock");
    CHECK(raw[3] == FS_MAX_FILES,        "max_files must be this build's FS_MAX_FILES");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_format_idempotent
 * Calling fs_format() twice must leave a valid empty filesystem.
 * ------------------------------------------------------------------------- */
static void test_format_idempotent(void)
{
    BEGIN_TEST(format_idempotent);
    fs_reset();
    fs_format();   /* second format */
    fs_init();
    CHECK(count_files() == 0, "file_count must still be 0 after double format");
    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_corrupt_superblock_triggers_reformat
 * Writing bad magic to the flash superblock, then calling fs_init(), must
 * trigger a reformat (the corrupted data cannot be mounted).
 * ------------------------------------------------------------------------- */
static void test_corrupt_superblock_triggers_reformat(void)
{
    BEGIN_TEST(corrupt_superblock_triggers_reformat);
    fs_reset();

    /* Create a file so we can confirm the reformat wiped it. */
    int fd = fs_open("keep.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0, "file creation must succeed before corruption test");
    fs_close(fd);
    CHECK(count_files() == 1, "one file must exist before corruption");

    /* Corrupt the magic word in the raw mock flash. */
    uint32_t *raw = (uint32_t *)mock_flash_buf;
    raw[0] = 0xDEADBEEFu;

    /* fs_init must detect the invalid magic and reformat. */
    fs_init();
    CHECK(count_files() == 0,
          "reformat on bad magic must result in empty filesystem");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_create_file_persists_metadata
 * Creating and closing a file must persist its metadata to flash.  After a
 * simulated reboot (fs_init() re-reading the flash), the file must still
 * appear in the directory listing.
 * ------------------------------------------------------------------------- */
static void test_create_file_persists_metadata(void)
{
    BEGIN_TEST(create_file_persists_metadata_across_reinit);
    fs_reset();

    int fd = fs_open("persist.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0, "file creation must succeed");
    fs_close(fd);

    /* Simulate reboot: re-read superblock from mock flash. */
    fs_init();
    CHECK(count_files() == 1, "file must still exist after fs_init re-reads flash");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_write_read_small
 * Write 32 bytes, close, reopen for read, verify the exact bytes come back.
 * ------------------------------------------------------------------------- */
static void test_write_read_small(void)
{
    BEGIN_TEST(write_read_small);
    fs_reset();

    const uint8_t payload[32] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,
    };

    int wfd = fs_open("small.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(wfd >= 0, "write open must succeed");
    int written = fs_write(wfd, payload, 32u);
    CHECK(written == 32, "write must report 32 bytes written");
    fs_close(wfd);

    int rfd = fs_open("small.txt", VFS_O_RDONLY);
    CHECK(rfd >= 0, "read open must succeed");

    uint8_t buf[32] = {0};
    int read_n = fs_read(rfd, buf, 32u);
    CHECK(read_n == 32, "read must return 32 bytes");
    CHECK(memcmp(buf, payload, 32) == 0, "read data must exactly match written data");
    fs_close(rfd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_write_read_max
 * Write exactly FS_MAX_FILE_DATA bytes (one full sector); verify full
 * round-trip.
 * ------------------------------------------------------------------------- */
static void test_write_read_max(void)
{
    BEGIN_TEST(write_read_full_sector);
    fs_reset();

    /* Build a recognisable fill pattern. */
    static uint8_t wbuf[FS_MAX_FILE_DATA];
    for (uint32_t i = 0u; i < FS_MAX_FILE_DATA; i++) {
        wbuf[i] = (uint8_t)(i & 0xFFu);
    }

    int wfd = fs_open("big.bin", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(wfd >= 0, "write open must succeed");
    int w = fs_write(wfd, wbuf, FS_MAX_FILE_DATA);
    CHECK(w == (int)FS_MAX_FILE_DATA, "full-sector write must succeed");
    fs_close(wfd);

    int rfd = fs_open("big.bin", VFS_O_RDONLY);
    CHECK(rfd >= 0, "read open must succeed");
    static uint8_t rbuf[FS_MAX_FILE_DATA];
    int r = fs_read(rfd, rbuf, FS_MAX_FILE_DATA);
    CHECK(r == (int)FS_MAX_FILE_DATA, "full-sector read must return all bytes");
    CHECK(memcmp(rbuf, wbuf, FS_MAX_FILE_DATA) == 0,
          "read data must match written data for full sector");
    fs_close(rfd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_write_over_max_fails
 * Writing more bytes than FS_MAX_FILE_DATA must be rejected.
 * ------------------------------------------------------------------------- */
static void test_write_over_max_fails(void)
{
    BEGIN_TEST(write_over_max_file_size_fails);
    fs_reset();

    int fd = fs_open("overflow.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(fd >= 0, "open must succeed");

    /* Write the full sector first. */
    static uint8_t fill[FS_MAX_FILE_DATA];
    memset(fill, 0xAA, FS_MAX_FILE_DATA);
    int w1 = fs_write(fd, fill, FS_MAX_FILE_DATA);
    CHECK(w1 == (int)FS_MAX_FILE_DATA, "first write (full sector) must succeed");

    /* Any further write must fail — no space remaining. */
    uint8_t extra = 0xFF;
    int w2 = fs_write(fd, &extra, 1u);
    CHECK(w2 == -1, "write past end of file capacity must return -1");
    fs_close(fd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_read_eof_returns_zero
 * Reading past end-of-file must return 0 (not an error).
 * ------------------------------------------------------------------------- */
static void test_read_eof_returns_zero(void)
{
    BEGIN_TEST(read_at_eof_returns_zero);
    fs_reset();

    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    int wfd = fs_open("eof.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(wfd, data, 10u);
    fs_close(wfd);

    int rfd = fs_open("eof.txt", VFS_O_RDONLY);
    uint8_t buf[20];
    int r = fs_read(rfd, buf, 10u);
    CHECK(r == 10, "first read must return 10 bytes");
    int r2 = fs_read(rfd, buf, 10u);
    CHECK(r2 == 0, "read past EOF must return 0");
    fs_close(rfd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_delete_removes_file
 * After fs_delete(), the file must no longer appear in the listing.
 * ------------------------------------------------------------------------- */
static void test_delete_removes_file(void)
{
    BEGIN_TEST(delete_removes_file_from_directory);
    fs_reset();

    int fd = fs_open("gone.txt", VFS_O_CREAT | VFS_O_WRONLY);
    fs_close(fd);
    CHECK(count_files() == 1, "file must exist before delete");

    int rc = fs_delete("gone.txt");
    CHECK(rc == 0, "fs_delete must return 0 on success");
    CHECK(count_files() == 0, "directory must be empty after delete");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_delete_reclaims_slot
 * After deleting a file, creating a new file with the same name must succeed.
 * ------------------------------------------------------------------------- */
static void test_delete_reclaims_slot(void)
{
    BEGIN_TEST(delete_reclaims_directory_slot);
    fs_reset();

    int fd = fs_open("reuse.txt", VFS_O_CREAT | VFS_O_WRONLY);
    fs_close(fd);
    fs_delete("reuse.txt");

    int fd2 = fs_open("reuse.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd2 >= 0, "creating a file with a previously deleted name must succeed");
    fs_close(fd2);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_max_files_limit
 * Creating FS_MAX_FILES files must succeed; the next creation must fail.
 * ------------------------------------------------------------------------- */
static void test_max_files_limit(void)
{
    BEGIN_TEST(max_files_limit);
    fs_reset();

    char name[16];
    bool all_ok = true;
    uint8_t dummy = 0xAA;
    for (int i = 0; i < (int)FS_MAX_FILES; i++) {
        /* Use short names to stay within FS_NAME_MAX. */
        snprintf(name, sizeof(name), "f%d", i);
        int fd = fs_open(name, VFS_O_CREAT | VFS_O_WRONLY);
        if (fd < 0) { all_ok = false; break; }
        /* Write one byte so dirty=true; fs_close will then flush and release
         * the scratch buffer, allowing the next file to be opened for write. */
        fs_write(fd, &dummy, 1u);
        fs_close(fd);
    }
    CHECK(all_ok, "creating FS_MAX_FILES files must all succeed");

    /* One more must fail — directory is full. */
    int overflow = fs_open("extra", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(overflow < 0, "creating a file beyond FS_MAX_FILES must return -1");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_file_name_boundary
 * A name of exactly FS_NAME_MAX - 1 characters must be stored and found.
 * ------------------------------------------------------------------------- */
static void test_file_name_boundary(void)
{
    BEGIN_TEST(file_name_at_max_length_boundary);
    fs_reset();

    /* FS_NAME_MAX includes the NUL terminator; max usable chars = 15. */
    char long_name[FS_NAME_MAX];
    memset(long_name, 'x', FS_NAME_MAX - 1u);
    long_name[FS_NAME_MAX - 1u] = '\0';

    int wfd = fs_open(long_name, VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(wfd >= 0, "open with max-length name must succeed");
    fs_close(wfd);

    int rfd = fs_open(long_name, VFS_O_RDONLY);
    CHECK(rfd >= 0, "re-opening max-length named file must succeed");
    fs_close(rfd);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_file_name_too_long_rejected  (issue #7)
 * A name of FS_NAME_MAX or more characters cannot be stored whole.  It used
 * to be stored cut to FS_NAME_MAX - 1, which the lookup never matched: each
 * create made a duplicate, and fs_delete() with the full name found none.
 * Now open fails and nothing is created.  Empty names fail too.
 * ------------------------------------------------------------------------- */
static void test_file_name_too_long_rejected(void)
{
    BEGIN_TEST(file_name_too_long_rejected);
    fs_reset();

    char long_name[FS_NAME_MAX + 1u];
    memset(long_name, 'y', FS_NAME_MAX);
    long_name[FS_NAME_MAX] = '\0';   /* FS_NAME_MAX chars: one too many */

    for (int i = 0; i < 2; i++) {
        int fd = fs_open(long_name, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        CHECK(fd < 0, "create with an over-long name must fail");
        if (fd >= 0) { fs_close(fd); }
    }
    CHECK(count_files() == 0, "no entry (truncated or not) is created");
    CHECK(fs_delete(long_name) == -1, "delete of an over-long name reports not found");

    /* The cut-down name is an ordinary file, unrelated to the long one. */
    char short_name[FS_NAME_MAX];
    memcpy(short_name, long_name, FS_NAME_MAX - 1u);
    short_name[FS_NAME_MAX - 1u] = '\0';
    int fd = fs_open(short_name, VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd >= 0, "the 15-char prefix is still a valid name");
    fs_close(fd);
    CHECK(fs_open(long_name, VFS_O_RDONLY) < 0, "long name does not alias its prefix");
    CHECK(count_files() == 1, "exactly one file exists");

    CHECK(fs_open("", VFS_O_CREAT | VFS_O_WRONLY) < 0, "empty name must fail");
    CHECK(fs_open("/", VFS_O_CREAT | VFS_O_WRONLY) < 0, "\"/\" alone must fail");
    CHECK(count_files() == 1, "failed opens created nothing");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_trunc_zeroes_content
 * Writing data, closing, reopening with VFS_O_TRUNC, then closing without
 * writing must produce a zero-length file.
 * ------------------------------------------------------------------------- */
static void test_trunc_zeroes_content(void)
{
    BEGIN_TEST(trunc_clears_file_content);
    fs_reset();

    /* Write 64 bytes of data. */
    uint8_t data[64];
    memset(data, 0xAB, 64);
    int w = fs_open("trunc.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w, data, 64u);
    fs_close(w);
    CHECK(file_size("trunc.txt") == 64, "file must be 64 bytes after first write");

    /* Reopen with TRUNC and close without writing. */
    int t = fs_open("trunc.txt", VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(t >= 0, "truncate re-open must succeed");
    fs_close(t);

    CHECK(file_size("trunc.txt") == 0, "file must be 0 bytes after truncate close");

    /* #18: the truncation used to live in RAM only; a reboot (fs_init)
     * brought the 64 bytes back. */
    fs_init();
    CHECK(file_size("trunc.txt") == 0, "file must still be 0 bytes after re-init");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_append_adds_to_end
 * Write N bytes, close, reopen with VFS_O_APPEND, write M bytes.  The total
 * size must be N + M and reading from the start must yield N + M bytes.
 * ------------------------------------------------------------------------- */
static void test_append_adds_to_end(void)
{
    BEGIN_TEST(append_extends_file);
    fs_reset();

    uint8_t part1[32], part2[16];
    memset(part1, 0x11, 32);
    memset(part2, 0x22, 16);

    int w1 = fs_open("app.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w1, part1, 32u);
    fs_close(w1);

    int w2 = fs_open("app.txt", VFS_O_WRONLY | VFS_O_APPEND);
    CHECK(w2 >= 0, "append open must succeed");
    fs_write(w2, part2, 16u);
    fs_close(w2);

    CHECK(file_size("app.txt") == 48, "file size must be 32 + 16 = 48 after append");

    uint8_t rbuf[48] = {0};
    int rfd = fs_open("app.txt", VFS_O_RDONLY);
    int r = fs_read(rfd, rbuf, 48u);
    fs_close(rfd);
    CHECK(r == 48, "read must return all 48 bytes");
    CHECK(memcmp(rbuf, part1, 32) == 0, "first 32 bytes must match part1");
    CHECK(memcmp(rbuf + 32, part2, 16) == 0, "last 16 bytes must match part2");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_open_rdonly_write_fails
 * fs_write on a read-only fd must return -1.
 * ------------------------------------------------------------------------- */
static void test_open_rdonly_write_fails(void)
{
    BEGIN_TEST(write_to_rdonly_fd_fails);
    fs_reset();

    /* Create and populate a file. */
    int w = fs_open("ro.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    uint8_t d[4] = {1,2,3,4};
    fs_write(w, d, 4u);
    fs_close(w);

    /* Open read-only and try to write. */
    int r = fs_open("ro.txt", VFS_O_RDONLY);
    CHECK(r >= 0, "read-only open must succeed");
    int rc = fs_write(r, d, 4u);
    CHECK(rc == -1, "fs_write on a read-only fd must return -1");
    fs_close(r);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_scratch_owner_enforces_single_writer
 * Opening a second file for writing while the first is still open must fail.
 * ------------------------------------------------------------------------- */
static void test_scratch_owner_enforces_single_writer(void)
{
    BEGIN_TEST(scratch_owner_single_writer_enforcement);
    fs_reset();

    int fd_a = fs_open("a.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd_a >= 0, "first write open must succeed");

    int fd_b = fs_open("b.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd_b < 0,
          "second write open while first is active must fail (scratch buffer busy)");

    /* Close without writing: fs_close releases the write buffer anyway. */
    uint8_t d = 0x55;
    fs_close(fd_a);

    /* Scratch buffer is now free; opening b.txt for write must succeed. */
    int fd_c = fs_open("b.txt", VFS_O_CREAT | VFS_O_WRONLY);
    CHECK(fd_c >= 0, "write open must succeed once scratch buffer is released");
    fs_write(fd_c, &d, 1u);
    fs_close(fd_c);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_metadata_change_keeps_pending_write
 * Creating and deleting files rewrites the superblock.  That must not touch
 * the write buffer of a file that is still open for writing.
 * ------------------------------------------------------------------------- */
static void test_metadata_change_keeps_pending_write(void)
{
    BEGIN_TEST(metadata_change_keeps_pending_write);
    fs_reset();

    int old = fs_open("old.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    uint8_t x = 0x11;
    fs_write(old, &x, 1u);
    fs_close(old);

    const uint8_t payload[8] = { 'p','e','n','d','i','n','g','!' };
    int wfd = fs_open("w.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(wfd >= 0, "write open must succeed");
    CHECK(fs_write(wfd, payload, 8u) == 8, "write must report 8 bytes");

    /* Create a new file (read-only open) and delete another while w.txt
     * still holds the write buffer. */
    int nfd = fs_open("new.txt", VFS_O_CREAT | VFS_O_RDONLY);
    CHECK(nfd >= 0, "creating another file must succeed");
    fs_close(nfd);
    CHECK(fs_delete("old.txt") == 0, "deleting another file must succeed");

    CHECK(fs_close(wfd) == 0, "close must commit");

    int rfd = fs_open("w.txt", VFS_O_RDONLY);
    uint8_t buf[8] = {0};
    CHECK(fs_read(rfd, buf, 8u) == 8, "read must return 8 bytes");
    CHECK(memcmp(buf, payload, 8) == 0,
          "pending data must survive superblock rewrites");
    fs_close(rfd);

    CHECK(fs_open("new.txt", VFS_O_RDONLY) >= 0, "new.txt must exist");

    END_TEST();
}


/* -------------------------------------------------------------------------
 * Helpers for the raw superblock in mock flash
 * ------------------------------------------------------------------------- */
#define RAW_SB  ((uint32_t *)mock_flash_buf)   /* superblock sector, as words */

/* Rewrite the on-flash superblock in the version-1 layout (no max_files:
 * entries start at byte 12) with an erased tail, as an older build left it. */
static void make_v1_superblock(void)
{
    uint8_t sector[FS_BLOCK_SIZE];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, mock_flash_buf, 12u);                    /* magic, version, count */
    memcpy(sector + 12u, mock_flash_buf + 16u,
           sizeof(fs_entry_t) * FS_MAX_FILES - sizeof(fs_entry_t));  /* all but the last */
    ((uint32_t *)sector)[1] = 1u;
    memcpy(mock_flash_buf, sector, sizeof(sector));
}

/* -------------------------------------------------------------------------
 * test_v1_superblock_converted  (#31)
 * A version-1 table has no max_files field.  It mounts with its files and
 * is written back as version 2.
 * ------------------------------------------------------------------------- */
static void test_v1_superblock_converted(void)
{
    BEGIN_TEST(v1_superblock_converted_in_place);
    fs_reset();

    const uint8_t data[5] = { 'h','e','l','l','o' };
    int w = fs_open("old.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w, data, 5u);
    fs_close(w);
    make_v1_superblock();

    fs_init();
    CHECK(count_files() == 1, "the v1 file must survive the conversion");
    CHECK(file_size("old.txt") == 5, "with its size");
    uint8_t buf[5] = {0};
    int r = fs_open("old.txt", VFS_O_RDONLY);
    CHECK(fs_read(r, buf, 5u) == 5 && memcmp(buf, data, 5) == 0, "and its data");
    fs_close(r);
    CHECK(RAW_SB[1] == FS_VERSION && RAW_SB[3] == FS_MAX_FILES,
          "the table must be written back as version 2");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_other_max_files_converted  (#31)
 * A table written with a larger FS_MAX_FILES (garbage past our end) or a
 * smaller one keeps only the entries that are valid here.
 * ------------------------------------------------------------------------- */
static void test_other_max_files_converted(void)
{
    BEGIN_TEST(other_max_files_table_converted);
    fs_reset();

    const char *names[3] = { "f0", "f1", "f2" };
    for (int i = 0; i < 3; i++) {
        fs_close(fs_open(names[i], VFS_O_CREAT | VFS_O_WRONLY));
    }

    /* As if written by a build with 2 slots: f2 (slot 2) is past its end. */
    RAW_SB[3] = 2u;
    fs_init();
    CHECK(count_files() == 2, "entries past the old table end are dropped");
    CHECK(file_size("f2") == -1, "f2 is gone");
    CHECK(RAW_SB[3] == FS_MAX_FILES, "the table is written back with our size");

    /* As if written by a build with more slots than ours: the extra
     * entries (here: erased 0xFF bytes) must not be read as files. */
    RAW_SB[3] = FS_MAX_FILES + 16u;
    fs_init();
    CHECK(count_files() == 2, "a larger table keeps the valid entries only");
    CHECK(RAW_SB[3] == FS_MAX_FILES, "and is written back with our size");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_reader_sees_committed_file  (#31)
 * While a write is pending, other fds and fs_list see the file as it is on
 * flash; the writer's own fd sees its pending data.
 * ------------------------------------------------------------------------- */
static void test_reader_sees_committed_file(void)
{
    BEGIN_TEST(reader_sees_committed_file_during_write);
    fs_reset();

    int w0 = fs_open("rw.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w0, (const uint8_t *)"old", 3u);
    fs_close(w0);

    int w = fs_open("rw.txt", VFS_O_RDWR | VFS_O_APPEND);
    CHECK(w >= 0, "append open must succeed");
    CHECK(fs_write(w, (const uint8_t *)"-new-data", 9u) == 9, "pending write of 9 bytes");

    CHECK(file_size("rw.txt") == 3, "fs_list must still show the committed 3 bytes");
    int r = fs_open("rw.txt", VFS_O_RDONLY);
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK(fs_read(r, buf, sizeof(buf)) == 3 && memcmp(buf, "old", 3) == 0,
          "another fd must read the committed 3 bytes, not 0xFF past them");
    fs_close(r);

    CHECK(fs_close(w) == 0, "close must commit");
    CHECK(file_size("rw.txt") == 12, "the new size is published by close");
    r = fs_open("rw.txt", VFS_O_RDONLY);
    memset(buf, 0, sizeof(buf));
    CHECK(fs_read(r, buf, sizeof(buf)) == 12 && memcmp(buf, "old-new-data", 12) == 0,
          "a reader opened after the commit sees all of it");
    fs_close(r);

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_deleted_file_fd_goes_stale  (#8)
 * An fd open on a deleted file stays allocated but fails I/O, so it cannot
 * be handed to (and then read or write) another file.
 * ------------------------------------------------------------------------- */
static void test_deleted_file_fd_goes_stale(void)
{
    BEGIN_TEST(deleted_file_fd_goes_stale);
    fs_reset();

    int w = fs_open("victim.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w, (const uint8_t *)"victim", 6u);
    fs_close(w);

    int r = fs_open("victim.txt", VFS_O_RDONLY);
    int p = fs_open("victim.txt", VFS_O_WRONLY | VFS_O_APPEND);
    CHECK(r >= 0 && p >= 0, "reader and writer open");
    CHECK(fs_delete("victim.txt") == 0, "delete while open must succeed");

    /* A new file: it must not get r's or p's fd slot. */
    int n = fs_open("other.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    CHECK(n >= 0, "a new writer can start: the deleted file's buffer was freed");
    CHECK(n != r && n != p, "the stale fds' slots are not reused");
    fs_write(n, (const uint8_t *)"other", 5u);
    fs_close(n);

    uint8_t buf[8];
    CHECK(fs_read(r, buf, sizeof(buf)) == -1, "read on a stale fd fails");
    CHECK(fs_write(p, (const uint8_t *)"x", 1u) == -1, "write on a stale fd fails");
    CHECK(fs_close(p) == 0 && fs_close(r) == 0, "close frees stale fds");
    CHECK(file_size("other.txt") == 5, "the other file is untouched");
    CHECK(file_size("victim.txt") == -1, "the pending write did not resurrect it");

    /* fs_format does the same to every open fd. */
    int f = fs_open("other.txt", VFS_O_RDONLY);
    fs_format();
    CHECK(fs_read(f, buf, sizeof(buf)) == -1, "an fd open across fs_format is stale");
    CHECK(fs_close(f) == 0, "and closes cleanly");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_close_programs_only_used_pages  (#39)
 * A small file programs one 256-byte page, not the whole 4 KB sector.
 * ------------------------------------------------------------------------- */
static void test_close_programs_only_used_pages(void)
{
    BEGIN_TEST(close_programs_only_used_pages);
    fs_reset();

    int w = fs_open("tiny.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w, (const uint8_t *)"ten bytes!", 10u);
    fs_close(w);

    /* tiny.txt is in slot 0, so its data is sector 1 of the mock region. */
    const uint8_t *sector = mock_flash_buf + FS_BLOCK_SIZE;
    CHECK(memcmp(sector, "ten bytes!", 10) == 0, "the data is on flash");
    bool erased = true;
    for (uint32_t i = FLASH_PAGE_SIZE; i < FS_BLOCK_SIZE; i++) {
        if (sector[i] != 0xFFu) { erased = false; break; }
    }
    CHECK(erased, "pages past the first are left erased");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_reopen_after_close_starts_at_zero
 * After closing a file, reopening it for read must start at position 0.
 * ------------------------------------------------------------------------- */
static void test_reopen_after_close_starts_at_zero(void)
{
    BEGIN_TEST(reopen_resets_position_to_zero);
    fs_reset();

    uint8_t data[20];
    for (int i = 0; i < 20; i++) data[i] = (uint8_t)i;

    int w = fs_open("pos.txt", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    fs_write(w, data, 20u);
    fs_close(w);

    /* First open: read 10 bytes. */
    int r1 = fs_open("pos.txt", VFS_O_RDONLY);
    uint8_t buf[10];
    fs_read(r1, buf, 10u);
    fs_close(r1);

    /* Second open: must start back at 0 and read all 20 bytes. */
    int r2 = fs_open("pos.txt", VFS_O_RDONLY);
    uint8_t buf2[20] = {0};
    int n = fs_read(r2, buf2, 20u);
    fs_close(r2);

    CHECK(n == 20, "read after reopen must return all 20 bytes from position 0");
    CHECK(buf2[0] == 0u && buf2[19] == 19u, "content must start from byte 0");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("picoOS filesystem — unit tests\n");
    printf("================================\n\n");

    test_format_creates_valid_superblock();
    test_format_idempotent();
    test_corrupt_superblock_triggers_reformat();
    test_create_file_persists_metadata();
    test_write_read_small();
    test_write_read_max();
    test_write_over_max_fails();
    test_read_eof_returns_zero();
    test_delete_removes_file();
    test_delete_reclaims_slot();
    test_max_files_limit();
    test_file_name_boundary();
    test_file_name_too_long_rejected();
    test_trunc_zeroes_content();
    test_append_adds_to_end();
    test_open_rdonly_write_fails();
    test_scratch_owner_enforces_single_writer();
    test_reopen_after_close_starts_at_zero();
    test_metadata_change_keeps_pending_write();
    test_v1_superblock_converted();
    test_other_max_files_converted();
    test_reader_sees_committed_file();
    test_deleted_file_fd_goes_stale();
    test_close_programs_only_used_pages();

    mock_flash_teardown();
    SUMMARY();
}
