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

#ifndef APPS_DEMO_H
#define APPS_DEMO_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Demo thread entry points
 * ------------------------------------------------------------------------- */

/*
 * demo_producer — generates counter values every 500 ms, places them in a
 *                 shared message queue and signals a semaphore.
 */
void demo_producer(void *arg);

/*
 * demo_consumer — waits on the semaphore, reads from the shared message
 *                 queue, and prints the value.
 */
void demo_consumer(void *arg);

/*
 * demo_sensor — simulates a temperature sensor reading every 2 seconds.
 */
void demo_sensor(void *arg);

/* -------------------------------------------------------------------------
 * Application table
 *
 * The shell 'run' command looks up entries in this table to spawn apps as
 * new processes.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    void      (*entry)(void *);
    uint8_t    priority;
} app_entry_t;

extern const app_entry_t app_table[];
extern const int         app_table_size;

#endif /* APPS_DEMO_H */
