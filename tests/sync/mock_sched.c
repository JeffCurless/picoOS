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
 * When PICOOS_LOCK_DEBUG is defined the file also provides:
 *   mock_yield_hook    — called once by sched_yield(); lets tests release a
 *                        held lock so the _dbg variants can retry and succeed.
 *   lock_deadlock_panic — captures panic arguments and longjmps back to the
 *                         test instead of halting; used to verify that the
 *                         deadlock detection fires correctly.
 */

#include "sched.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

#ifdef PICOOS_LOCK_DEBUG
#include <setjmp.h>
#endif

/* -------------------------------------------------------------------------
 * Shared state
 * ------------------------------------------------------------------------- */

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

tcb_t * volatile current_tcb[2] = { &mock_tcb, NULL };

/* =========================================================================
 * Yield hook
 *
 * Set mock_yield_hook to a callback before calling a lock primitive that
 * will block.  sched_yield() fires the hook exactly once (then clears it).
 * This lets deadlock tests release a held lock during the blocked-wait path
 * so that the lock function can retry and eventually acquire — all without
 * a real second thread.
 * ========================================================================= */
void (*mock_yield_hook)(void) = NULL;

/* =========================================================================
 * Core scheduler stubs used by sync.c
 * ========================================================================= */

void sched_block(tcb_t *t)  { if (t) t->state = THREAD_BLOCKED; }
void sched_unblock(tcb_t *t){ if (t) t->state = THREAD_READY;   }

void sched_yield(void)
{
    if (mock_yield_hook) {
        void (*h)(void) = mock_yield_hook;
        mock_yield_hook = NULL;   /* fire once */
        h();
    }
}

/* =========================================================================
 * Remaining sched.h declarations — unused by sync.c but required by the
 * linker because sched.h is transitively included.
 * ========================================================================= */
void    sched_init(void)               {}
void    sched_start(void)              {}
void    sched_tick(void)               {}
void    sched_sleep(uint32_t ms)       { (void)ms; }
tcb_t  *sched_next_thread(void)        { return NULL; }
void    sched_add_thread(tcb_t *t)     { (void)t; }
void    sched_remove_thread(tcb_t *t)  { (void)t; }

/* =========================================================================
 * Deadlock panic stub  (PICOOS_LOCK_DEBUG builds only)
 *
 * Rather than halting the system, this mock captures the panic arguments
 * into test-visible globals and longjmps back to the test's setjmp point.
 * The __attribute__((noreturn)) is satisfied: longjmp never returns to the
 * caller, matching the contract declared in sched.h.
 *
 * Usage pattern in a test:
 *   mock_lock_panic_fired = false;
 *   if (setjmp(mock_lock_panic_jmp) == 0) {
 *       // call something that should time out / deadlock
 *   } else {
 *       CHECK(mock_lock_panic_fired, "panic must fire");
 *       CHECK(strcmp(mock_lock_panic_type, "spinlock") == 0, "...type");
 *   }
 * ========================================================================= */
#ifdef PICOOS_LOCK_DEBUG

jmp_buf     mock_lock_panic_jmp;
bool        mock_lock_panic_fired     = false;
char        mock_lock_panic_type[32]  = {0};
int32_t     mock_lock_panic_wait_tid  = 0;
int32_t     mock_lock_panic_hold_tid  = 0;
const char *mock_lock_panic_wait_file = NULL;
const char *mock_lock_panic_hold_file = NULL;

void __attribute__((noreturn)) lock_deadlock_panic(
    const char *lock_type,
    const char *wait_file, int wait_line,
    uint32_t    wait_tid,  const char *wait_name,
    const char *hold_file, int hold_line, int32_t hold_tid)
{
    (void)wait_line; (void)hold_line; (void)wait_name;

    mock_lock_panic_fired    = true;
    mock_lock_panic_wait_tid = (int32_t)wait_tid;
    mock_lock_panic_hold_tid = hold_tid;
    mock_lock_panic_wait_file = wait_file;
    mock_lock_panic_hold_file = hold_file;

    strncpy(mock_lock_panic_type, lock_type ? lock_type : "?",
            sizeof(mock_lock_panic_type) - 1u);
    mock_lock_panic_type[sizeof(mock_lock_panic_type) - 1u] = '\0';

    longjmp(mock_lock_panic_jmp, 1);
}

#endif /* PICOOS_LOCK_DEBUG */
