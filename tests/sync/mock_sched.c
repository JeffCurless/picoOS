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
 * tests/sync/mock_sched.c — scheduler stubs for host-native sync tests.
 *
 * sync.c depends on sched_block(), sched_unblock(), sched_yield(), and the
 * current_tcb global.  This file provides minimal stub implementations so
 * that sync.c can be compiled and exercised on a host without any ARM or
 * Pico SDK dependencies.
 *
 * Scope limitation: sched_yield() is a no-op here, so any sync path that
 * blocks (mutex held, semaphore count==0, full queue, unsatisfied flags)
 * would loop forever.  Tests must exercise only non-blocking paths.
 */

#include "sched.h"
#include "task.h"
#include <stddef.h>

volatile uint32_t tick_count = 0;

/* Single dummy TCB representing the one "running" thread in host tests. */
static tcb_t mock_tcb = {
    .tid      = 1u,
    .pid      = 1u,
    .state    = THREAD_RUNNING,
    .priority = 0u,
    .name     = "test",
    .next     = NULL,
};

tcb_t * volatile current_tcb = &mock_tcb;

/* Core stubs used by sync.c ------------------------------------------------ */
void sched_block(tcb_t *t)             { if (t) t->state = THREAD_BLOCKED; }
void sched_unblock(tcb_t *t)           { if (t) t->state = THREAD_READY; }
void sched_yield(void)                 {}

/* Remaining sched.h declarations — unused by sync.c but required by the
 * linker because sched.h is transitively included. */
void    sched_init(void)               {}
void    sched_start(void)              {}
void    sched_tick(void)               {}
void    sched_sleep(uint32_t ms)       { (void)ms; }
tcb_t  *sched_next_thread(void)        { return NULL; }
void    sched_add_thread(tcb_t *t)     { (void)t; }
void    sched_remove_thread(tcb_t *t)  { (void)t; }
