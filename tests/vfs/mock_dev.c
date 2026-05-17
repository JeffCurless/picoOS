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
 * tests/vfs/mock_dev.c — device-layer stub for host-native vfs tests.
 *
 * All functions succeed (return 0 / bytes requested).  Call counters let
 * tests verify that vfs.c dispatches to the device layer correctly.
 */

#include "mock_dev.h"
#include "dev.h"
#include <stddef.h>

int      mock_dev_open_calls  = 0;
int      mock_dev_close_calls = 0;
dev_id_t mock_last_dev_opened = DEV_COUNT;

void mock_dev_reset(void)
{
    mock_dev_open_calls  = 0;
    mock_dev_close_calls = 0;
    mock_last_dev_opened = DEV_COUNT;
}

void      dev_init(void)          {}
device_t *dev_get(dev_id_t id)    { (void)id; return NULL; }

int dev_open(dev_id_t id)
{
    mock_dev_open_calls++;
    mock_last_dev_opened = id;
    return 0;
}

int dev_read(dev_id_t id, uint8_t *buf, uint32_t len)
{
    (void)id; (void)buf;
    return (int)len;
}

int dev_write(dev_id_t id, const uint8_t *buf, uint32_t len)
{
    (void)id; (void)buf;
    return (int)len;
}

int dev_ioctl(dev_id_t id, uint32_t cmd, void *arg)
{
    (void)id; (void)cmd; (void)arg;
    return 0;
}

void dev_close(dev_id_t id)
{
    mock_dev_close_calls++;
    (void)id;
}
