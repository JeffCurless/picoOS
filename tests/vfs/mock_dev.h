/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 */

#pragma once
#include "dev.h"

extern int      mock_dev_open_calls;
extern int      mock_dev_close_calls;
extern dev_id_t mock_last_dev_opened;

void mock_dev_reset(void);
