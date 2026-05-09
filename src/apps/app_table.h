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

/*
 * app_table.h — stable application registration ABI
 *
 * This is the only picoOS header an external application file needs to include
 * in order to register apps with the shell 'run' command.  It is intentionally
 * kept free of kernel internals so that projects using picoOS as a git submodule
 * can include it without depending on any other picoOS implementation detail.
 *
 * To register your apps, provide a translation unit that defines:
 *
 *   const app_entry_t app_table[]    = { { "name", entry_fn, priority }, ... };
 *   const int         app_table_size = sizeof(app_table) / sizeof(app_table[0]);
 *
 * When building picoOS standalone, apps/demo.c provides this definition.
 * When using picoOS as a submodule, set PICOOS_INCLUDE_DEMO_APPS=OFF and
 * supply your own translation unit via the PICOOS_APP_SOURCES cmake variable.
 */

#ifndef APPS_APP_TABLE_H
#define APPS_APP_TABLE_H

#include <stdint.h>

typedef struct {
    const char *name;
    void      (*entry)(void *);
    uint8_t    priority;
} app_entry_t;

extern const app_entry_t app_table[];
extern const int         app_table_size;

#endif /* APPS_APP_TABLE_H */
