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
 * tests/fs/mock_flash.h — RAM-backed flash mock for host-native fs tests.
 *
 * mock_flash_init()     — allocate the buffer; set all bytes to 0xFF (erased).
 *                         Also sets host_xip_base so XIP pointer arithmetic in
 *                         fs.c resolves to the correct offset within the buffer.
 * mock_flash_teardown() — free the buffer.
 *
 * The real flash_range_erase / flash_range_program are provided by mock_flash.c
 * and operate on the RAM buffer.  arch.h guards its no-op stubs with
 * #ifndef HOST_TEST so there is no redefinition conflict.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/* Pointer to the mock flash buffer.  mock_flash_buf[0] corresponds to
 * the first byte of the FS flash region (at FS_FLASH_OFFSET in real flash). */
extern uint8_t *mock_flash_buf;

void mock_flash_init(void);
void mock_flash_teardown(void);
