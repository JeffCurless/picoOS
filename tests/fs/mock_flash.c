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
 * tests/fs/mock_flash.c — RAM-backed flash for host-native fs.c tests.
 *
 * Layout of mock_flash_buf (each row = one FS_BLOCK_SIZE sector):
 *
 *   mock_flash_buf[0]                       : superblock
 *   mock_flash_buf[FS_BLOCK_SIZE]           : file index 0 data
 *   mock_flash_buf[2*FS_BLOCK_SIZE]         : file index 1 data
 *   ...
 *   mock_flash_buf[FS_MAX_FILES*FS_BLOCK_SIZE] : file index (FS_MAX_FILES-1) data
 *
 * host_xip_base is set so that XIP_BASE + FS_FLASH_OFFSET == mock_flash_buf.
 * All flash_range_erase / flash_range_program calls receive absolute offsets
 * from the START of flash (same convention as the real RP2040 SDK).
 */

#include "mock_flash.h"
#include "fs.h"     /* FS_FLASH_OFFSET, FS_BLOCK_SIZE, FS_MAX_FILES */

#include <stdlib.h>
#include <string.h>

/* See arch.h HOST_TEST block — extern declared there. */
uintptr_t host_xip_base;

uint8_t *mock_flash_buf = NULL;

/* Total bytes needed: superblock sector + one sector per file. */
#define MOCK_FLASH_BYTES  ((1u + FS_MAX_FILES) * FS_BLOCK_SIZE)

void mock_flash_init(void)
{
    if (mock_flash_buf) {
        free(mock_flash_buf);
    }
    mock_flash_buf = (uint8_t *)malloc(MOCK_FLASH_BYTES);

    /* Erased flash reads as 0xFF. */
    memset(mock_flash_buf, 0xFF, MOCK_FLASH_BYTES);

    /* XIP arithmetic in fs.c: XIP_BASE + FS_FLASH_OFFSET + sector_offset.
     * We want that to resolve to mock_flash_buf[sector_offset].
     * Therefore: host_xip_base = (uintptr_t)mock_flash_buf - FS_FLASH_OFFSET. */
    host_xip_base = (uintptr_t)mock_flash_buf - FS_FLASH_OFFSET;
}

void mock_flash_teardown(void)
{
    free(mock_flash_buf);
    mock_flash_buf = NULL;
}

/* -------------------------------------------------------------------------
 * Flash function implementations (override arch.h no-op stubs via HOST_TEST)
 * ------------------------------------------------------------------------- */

/* offset is an absolute byte offset from the START of flash (RP2040 convention).
 * The FS region starts at FS_FLASH_OFFSET, so subtract that to get the index
 * into mock_flash_buf. */

void flash_range_erase(uint32_t offset, size_t count)
{
    memset(mock_flash_buf + (offset - FS_FLASH_OFFSET), 0xFF, count);
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    memcpy(mock_flash_buf + (offset - FS_FLASH_OFFSET), data, count);
}
