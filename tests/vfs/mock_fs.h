/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 */

#pragma once

extern int mock_fs_open_calls;
extern int mock_fs_close_calls;
extern int mock_fs_next_fd;

void mock_fs_reset(void);
