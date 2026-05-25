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
 * tests/sync/test_sync.c — host-native tests for the picoOS sync primitives.
 *
 * Scheduler calls (sched_block, sched_unblock, sched_yield) are provided by
 * mock_sched.c as stubs.  Because sched_yield() is a no-op, tests cover only
 * the non-blocking fast paths:
 *
 *   Spinlock   — acquire / release, IRQ-save variant
 *   Mutex      — init state, lock-free, unlock, re-lock
 *   Semaphore  — init count, signal, wait when count > 0
 *   Event flags — set, clear, wait when condition already satisfied
 *   Message queue — send / recv round-trips, FIFO order, capacity, size clamp
 *
 * Build (from tests/):
 *   cmake -B build && make -C build
 *   ./build/test_sync
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "sync.h"
#include "task.h"    /* tcb_t, current_tcb */
#include "../framework.h"

/* External symbols provided by mock_sched.c -------------------------------- */
extern void (*mock_yield_hook)(void);

#ifdef PICOOS_LOCK_DEBUG
#include <setjmp.h>
extern jmp_buf     mock_lock_panic_jmp;
extern bool        mock_lock_panic_fired;
extern char        mock_lock_panic_type[32];
extern int32_t     mock_lock_panic_wait_tid;
extern int32_t     mock_lock_panic_hold_tid;
extern const char *mock_lock_panic_wait_file;
extern const char *mock_lock_panic_hold_file;
#endif

/* -------------------------------------------------------------------------
 * Spinlock tests
 * ------------------------------------------------------------------------- */

static void test_spinlock_default_state(void)
{
    BEGIN_TEST(spinlock_default_state_is_unlocked);
    spinlock_t s = {0};
    CHECK(s.lock == 0u, "zero-initialised spinlock must have lock == 0 (free)");
    END_TEST();
}

static void test_spinlock_acquire_sets_lock(void)
{
    BEGIN_TEST(spinlock_acquire_sets_lock_to_1);
    spinlock_t s = {0};
    spinlock_acquire(&s);
    CHECK(s.lock == 1u, "lock must be 1 after acquire");
    spinlock_release(&s);
    END_TEST();
}

static void test_spinlock_release_clears_lock(void)
{
    BEGIN_TEST(spinlock_release_clears_lock_to_0);
    spinlock_t s = {0};
    spinlock_acquire(&s);
    spinlock_release(&s);
    CHECK(s.lock == 0u, "lock must be 0 after release");
    END_TEST();
}

static void test_spinlock_irq_acquire_release(void)
{
    BEGIN_TEST(spinlock_irq_acquire_release_round_trip);
    spinlock_t s = {0};
    uint32_t saved = spinlock_irq_acquire(&s);
    CHECK(s.lock == 1u, "irq_acquire must set lock to 1");
    spinlock_irq_release(&s, saved);
    CHECK(s.lock == 0u, "irq_release must set lock back to 0");
    END_TEST();
}

static void test_spinlock_reacquire_after_release(void)
{
    BEGIN_TEST(spinlock_reacquire_after_release);
    spinlock_t s = {0};
    spinlock_acquire(&s);
    spinlock_release(&s);
    spinlock_acquire(&s);
    CHECK(s.lock == 1u, "lock must be 1 on second acquire");
    spinlock_release(&s);
    END_TEST();
}

/* -------------------------------------------------------------------------
 * Mutex tests
 * ------------------------------------------------------------------------- */

static void test_mutex_init_state(void)
{
    BEGIN_TEST(mutex_init_state);
    kmutex_t m;
    kmutex_init(&m);
    CHECK(m.owner_tid == -1,    "owner_tid must be -1 after init");
    CHECK(m.count     == 0u,    "count must be 0 after init");
    CHECK(m.waiters   == NULL,  "waiters must be NULL after init");
    CHECK(m.spin.lock == 0u,    "internal spinlock must be free after init");
    END_TEST();
}

static void test_mutex_lock_when_free(void)
{
    BEGIN_TEST(mutex_lock_when_free);
    kmutex_t m;
    kmutex_init(&m);
    kmutex_lock(&m);
    CHECK(m.owner_tid == (int32_t)CURRENT_TCB->tid,
          "owner_tid must equal current thread's tid after lock");
    CHECK(m.count == 1u, "count must be 1 after lock");
    kmutex_unlock(&m);
    END_TEST();
}

static void test_mutex_unlock(void)
{
    BEGIN_TEST(mutex_unlock_clears_owner);
    kmutex_t m;
    kmutex_init(&m);
    kmutex_lock(&m);
    kmutex_unlock(&m);
    CHECK(m.owner_tid == -1, "owner_tid must be -1 after unlock");
    CHECK(m.count     == 0u, "count must be 0 after unlock");
    END_TEST();
}

static void test_mutex_relock_after_unlock(void)
{
    BEGIN_TEST(mutex_relock_after_unlock);
    kmutex_t m;
    kmutex_init(&m);
    kmutex_lock(&m);
    kmutex_unlock(&m);
    kmutex_lock(&m);
    CHECK(m.owner_tid == (int32_t)CURRENT_TCB->tid,
          "owner_tid must be set again after second lock");
    kmutex_unlock(&m);
    END_TEST();
}

/* -------------------------------------------------------------------------
 * Semaphore tests
 * ------------------------------------------------------------------------- */

static void test_semaphore_init_count(void)
{
    BEGIN_TEST(semaphore_init_count);
    ksemaphore_t s;
    ksemaphore_init(&s, 5);
    CHECK(s.count   == 5,    "count must equal the initial value");
    CHECK(s.waiters == NULL, "waiters must be NULL after init");
    END_TEST();
}

static void test_semaphore_signal_increments(void)
{
    BEGIN_TEST(semaphore_signal_increments_count);
    ksemaphore_t s;
    ksemaphore_init(&s, 0);
    ksemaphore_signal(&s);
    CHECK(s.count == 1, "count must be 1 after one signal");
    ksemaphore_signal(&s);
    CHECK(s.count == 2, "count must be 2 after two signals");
    END_TEST();
}

static void test_semaphore_wait_decrements(void)
{
    BEGIN_TEST(semaphore_wait_decrements_count);
    ksemaphore_t s;
    ksemaphore_init(&s, 3);
    ksemaphore_wait(&s);
    CHECK(s.count == 2, "count must be 2 after one wait on count=3");
    ksemaphore_wait(&s);
    CHECK(s.count == 1, "count must be 1 after two waits on count=3");
    END_TEST();
}

static void test_semaphore_signal_wait_neutral(void)
{
    BEGIN_TEST(semaphore_signal_then_wait_restores_count);
    ksemaphore_t s;
    ksemaphore_init(&s, 2);
    ksemaphore_signal(&s);   /* count → 3 */
    ksemaphore_wait(&s);     /* count → 2 */
    CHECK(s.count == 2, "count must return to initial value after signal + wait");
    END_TEST();
}

static void test_semaphore_multi_signal_multi_wait(void)
{
    BEGIN_TEST(semaphore_multi_signal_multi_wait);
    ksemaphore_t s;
    ksemaphore_init(&s, 0);

    for (int i = 0; i < 8; i++) ksemaphore_signal(&s);
    CHECK(s.count == 8, "count must be 8 after 8 signals");

    for (int i = 0; i < 8; i++) ksemaphore_wait(&s);
    CHECK(s.count == 0, "count must return to 0 after 8 waits");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * Event flags tests
 * ------------------------------------------------------------------------- */

static void test_event_flags_init_zero(void)
{
    BEGIN_TEST(event_flags_init_zero);
    event_flags_t e;
    event_flags_init(&e);
    CHECK(e.flags   == 0u,   "flags must be 0 after init");
    CHECK(e.waiters == NULL, "waiters must be NULL after init");
    END_TEST();
}

static void test_event_flags_set_individual_bits(void)
{
    BEGIN_TEST(event_flags_set_individual_bits);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0x01u);
    CHECK(e.flags == 0x01u, "bit 0 must be set");

    event_flags_set(&e, 0x8000u);
    CHECK(e.flags == 0x8001u, "bit 15 must be added without clearing bit 0");

    event_flags_set(&e, 0x80000000u);
    CHECK(e.flags == 0x80008001u, "bit 31 must be added without clearing others");

    END_TEST();
}

static void test_event_flags_clear_bits(void)
{
    BEGIN_TEST(event_flags_clear_bits);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0xFFFFFFFFu);
    event_flags_clear(&e, 0x0Fu);
    CHECK((e.flags & 0x0Fu) == 0u,
          "cleared bits must be 0");
    CHECK((e.flags & ~0x0Fu) == (0xFFFFFFFFu & ~0x0Fu),
          "uncleaned bits must remain set");

    END_TEST();
}

static void test_event_flags_set_does_not_clear_others(void)
{
    BEGIN_TEST(event_flags_set_does_not_clear_other_bits);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0xAAAAAAAAu);
    event_flags_set(&e, 0x55555555u);
    CHECK(e.flags == 0xFFFFFFFFu, "OR semantics: all bits must be set");

    END_TEST();
}

static void test_event_flags_all_32_bits(void)
{
    BEGIN_TEST(event_flags_all_32_bits_set_and_clear);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0xFFFFFFFFu);
    CHECK(e.flags == 0xFFFFFFFFu, "all 32 bits must be set");

    event_flags_clear(&e, 0xFFFFFFFFu);
    CHECK(e.flags == 0u, "all 32 bits must be cleared");

    END_TEST();
}

static void test_event_flags_wait_any_already_set(void)
{
    BEGIN_TEST(event_flags_wait_any_condition_already_met);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0x01u);
    uint32_t result = event_flags_wait(&e, 0x07u, false);
    CHECK((result & 0x07u) != 0u,
          "wait-for-any must return immediately when any bit in mask is set");

    END_TEST();
}

static void test_event_flags_wait_all_already_set(void)
{
    BEGIN_TEST(event_flags_wait_all_condition_already_met);
    event_flags_t e;
    event_flags_init(&e);

    event_flags_set(&e, 0x07u);   /* set bits 0, 1, 2 */
    uint32_t result = event_flags_wait(&e, 0x07u, true);
    CHECK((result & 0x07u) == 0x07u,
          "wait-for-all must return immediately when all mask bits are set");

    END_TEST();
}

static void test_event_flags_wait_any_partial_match(void)
{
    BEGIN_TEST(event_flags_wait_any_partial_match_satisfied);
    event_flags_t e;
    event_flags_init(&e);

    /* Set only bit 1 out of a 3-bit wait mask. */
    event_flags_set(&e, 0x02u);
    uint32_t result = event_flags_wait(&e, 0x07u, false);
    CHECK((result & 0x02u) != 0u,
          "wait-for-any must succeed when at least one mask bit is set");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * Message queue tests
 * ------------------------------------------------------------------------- */

static void test_mqueue_init_state(void)
{
    BEGIN_TEST(mqueue_init_state);
    mqueue_t q;
    mqueue_init(&q, 16u);
    CHECK(q.count        == 0u,   "count must be 0 after init");
    CHECK(q.head         == 0u,   "head must be 0 after init");
    CHECK(q.tail         == 0u,   "tail must be 0 after init");
    CHECK(q.msg_size     == 16u,  "msg_size must match requested size");
    CHECK(q.send_waiters == NULL, "send_waiters must be NULL after init");
    CHECK(q.recv_waiters == NULL, "recv_waiters must be NULL after init");
    END_TEST();
}

static void test_mqueue_size_clamp(void)
{
    BEGIN_TEST(mqueue_size_clamped_to_MQ_MSG_SIZE);
    mqueue_t q;
    mqueue_init(&q, MQ_MSG_SIZE + 100u);
    CHECK(q.msg_size == MQ_MSG_SIZE,
          "msg_size must be clamped to MQ_MSG_SIZE when request exceeds limit");
    END_TEST();
}

static void test_mqueue_send_recv_single(void)
{
    BEGIN_TEST(mqueue_send_recv_single_message);
    mqueue_t q;
    mqueue_init(&q, 8u);

    uint8_t send_buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t recv_buf[8] = {0};

    mqueue_send(&q, send_buf);
    CHECK(q.count == 1u, "count must be 1 after one send");

    mqueue_recv(&q, recv_buf);
    CHECK(q.count == 0u, "count must be 0 after one recv");
    CHECK(memcmp(send_buf, recv_buf, 8) == 0,
          "received bytes must match sent bytes");

    END_TEST();
}

static void test_mqueue_fifo_order(void)
{
    BEGIN_TEST(mqueue_fifo_order_preserved);
    mqueue_t q;
    mqueue_init(&q, sizeof(uint32_t));

    uint32_t vals[4] = {10u, 20u, 30u, 40u};
    for (int i = 0; i < 4; i++) mqueue_send(&q, &vals[i]);

    bool order_ok = true;
    for (int i = 0; i < 4; i++) {
        uint32_t out = 0;
        mqueue_recv(&q, &out);
        if (out != vals[i]) order_ok = false;
    }
    CHECK(order_ok, "messages must be received in the same order they were sent");
    CHECK(q.count == 0u, "count must be 0 after draining all messages");

    END_TEST();
}

static void test_mqueue_fill_to_capacity(void)
{
    BEGIN_TEST(mqueue_fill_to_MQ_MAX_MSG_capacity);
    mqueue_t q;
    mqueue_init(&q, 4u);

    uint32_t val = 0xBEEFu;
    for (uint32_t i = 0u; i < MQ_MAX_MSG; i++) {
        mqueue_send(&q, &val);
    }
    CHECK(q.count == MQ_MAX_MSG,
          "count must equal MQ_MAX_MSG after filling the queue");

    END_TEST();
}

static void test_mqueue_drain_to_empty(void)
{
    BEGIN_TEST(mqueue_drain_to_empty);
    mqueue_t q;
    mqueue_init(&q, 4u);

    uint32_t val = 0xCAFEu;
    for (uint32_t i = 0u; i < MQ_MAX_MSG; i++) mqueue_send(&q, &val);

    uint32_t out;
    for (uint32_t i = 0u; i < MQ_MAX_MSG; i++) mqueue_recv(&q, &out);
    CHECK(q.count == 0u, "count must be 0 after draining all messages");

    END_TEST();
}

static void test_mqueue_msg_content_integrity(void)
{
    BEGIN_TEST(mqueue_message_content_integrity);
    mqueue_t q;
    mqueue_init(&q, 16u);

    /* Send MQ_MAX_MSG different payloads; receive and verify each. */
    uint8_t send_bufs[MQ_MAX_MSG][16];
    for (uint32_t i = 0u; i < MQ_MAX_MSG; i++) {
        memset(send_bufs[i], (int)(i + 1u), 16);
        mqueue_send(&q, send_bufs[i]);
    }

    bool all_ok = true;
    for (uint32_t i = 0u; i < MQ_MAX_MSG; i++) {
        uint8_t recv_buf[16] = {0};
        mqueue_recv(&q, recv_buf);
        if (memcmp(recv_buf, send_bufs[i], 16) != 0) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok, "all received payloads must exactly match the sent payloads");

    END_TEST();
}

static void test_mqueue_interleaved_send_recv(void)
{
    BEGIN_TEST(mqueue_interleaved_send_recv);
    mqueue_t q;
    mqueue_init(&q, sizeof(uint32_t));

    /* Alternate: send two, recv one, send one, recv two. */
    uint32_t a = 1u, b = 2u, c = 3u;
    mqueue_send(&q, &a);
    mqueue_send(&q, &b);
    uint32_t out;
    mqueue_recv(&q, &out);
    CHECK(out == 1u, "first recv must return first sent value");
    mqueue_send(&q, &c);
    mqueue_recv(&q, &out);
    CHECK(out == 2u, "second recv must return second sent value");
    mqueue_recv(&q, &out);
    CHECK(out == 3u, "third recv must return third sent value");
    CHECK(q.count == 0u, "queue must be empty after all receives");

    END_TEST();
}

/* =========================================================================
 * Deadlock detection tests  (PICOOS_LOCK_DEBUG only)
 *
 * These tests verify the PICOOS_LOCK_DEBUG instrumentation added to sync.c:
 *
 *   debug init fields    — _init() functions zero all debug fields.
 *   debug acquire fields — _dbg variants set acq_* on the lock and clear
 *                          on release.
 *   macro routing        — the kmutex_lock() macro calls the _dbg variant.
 *   blocked-path fields  — _dbg sets blk_* on the TCB before sched_block().
 *   spinlock timeout     — spinlock_irq_acquire() calls lock_deadlock_panic()
 *                          after PICOOS_LOCK_TIMEOUT_MS of spinning.
 * ========================================================================= */
#ifdef PICOOS_LOCK_DEBUG

/* State used by the yield hook to release a mutex during the blocked path. */
static kmutex_t *_hook_mutex_ptr;
static uint64_t  _hook_blk_time_us;
static const char *_hook_blk_file;
static int       _hook_blk_line;
static const char *_hook_blk_what;
static int32_t   _hook_blk_holder_tid;

static void mutex_capture_and_release_hook(void)
{
    /* Capture TCB debug fields while they're live (before the clear). */
    _hook_blk_time_us    = CURRENT_TCB->blk_time_us;
    _hook_blk_file       = CURRENT_TCB->blk_file;
    _hook_blk_line       = CURRENT_TCB->blk_line;
    _hook_blk_what       = CURRENT_TCB->blk_what;
    _hook_blk_holder_tid = CURRENT_TCB->blk_holder_tid;
    /* Release the mutex so the next iteration of kmutex_lock_dbg succeeds. */
    if (_hook_mutex_ptr) {
        _hook_mutex_ptr->owner_tid = -1;
    }
}

/* --- debug init fields --------------------------------------------------- */
static void test_lock_debug_mutex_init_fields(void)
{
    BEGIN_TEST(lock_debug_mutex_init_zeros_all_debug_fields);
    kmutex_t m;
    kmutex_init(&m);
    CHECK(m.spin.acq_file == NULL,  "spinlock acq_file must be NULL after init");
    CHECK(m.spin.acq_line == 0,     "spinlock acq_line must be 0 after init");
    CHECK(m.spin.acq_tid  == -1,    "spinlock acq_tid must be -1 after init");
    CHECK(m.acq_file      == NULL,  "mutex acq_file must be NULL after init");
    CHECK(m.acq_line      == 0,     "mutex acq_line must be 0 after init");
    CHECK(m.acq_time_us   == 0u,    "mutex acq_time_us must be 0 after init");
    END_TEST();
}

static void test_lock_debug_semaphore_init_fields(void)
{
    BEGIN_TEST(lock_debug_semaphore_init_zeros_spinlock_debug_fields);
    ksemaphore_t s;
    ksemaphore_init(&s, 3);
    CHECK(s.spin.acq_file == NULL,  "spinlock acq_file must be NULL after init");
    CHECK(s.spin.acq_line == 0,     "spinlock acq_line must be 0 after init");
    CHECK(s.spin.acq_tid  == -1,    "spinlock acq_tid must be -1 after init");
    END_TEST();
}

/* --- spinlock debug tracking --------------------------------------------- */
static void test_lock_debug_spinlock_acq_tid(void)
{
    BEGIN_TEST(lock_debug_spinlock_records_and_clears_holder_tid);
    spinlock_t s = {0};
    s.lock     = 0u;
    s.acq_tid  = -1;
    s.acq_file = NULL;
    s.acq_line = 0;

    uint32_t saved = spinlock_irq_acquire(&s);
    CHECK(s.acq_tid == (int32_t)CURRENT_TCB->tid,
          "acq_tid must equal current TID after acquire");
    spinlock_irq_release(&s, saved);
    CHECK(s.acq_tid  == -1,   "acq_tid must be -1 after release");
    CHECK(s.acq_file == NULL, "acq_file must be NULL after release");
    END_TEST();
}

/* --- mutex acquire / release debug fields -------------------------------- */
static void test_lock_debug_mutex_acq_fields_on_lock(void)
{
    BEGIN_TEST(lock_debug_mutex_sets_acq_fields_on_lock_and_clears_on_unlock);
    kmutex_t m;
    kmutex_init(&m);

    /* Use the _dbg variant directly so we control the file/line. */
    kmutex_lock_dbg(&m, "sentinel_file.c", 999);

    CHECK(m.acq_file != NULL,                       "acq_file must not be NULL after lock");
    CHECK(strcmp(m.acq_file, "sentinel_file.c") == 0, "acq_file must match argument");
    CHECK(m.acq_line == 999,                        "acq_line must match argument");
    CHECK(m.acq_time_us != 0u,                      "acq_time_us must be non-zero after lock");
    CHECK(m.owner_tid == (int32_t)CURRENT_TCB->tid, "owner_tid must be current TID");

    kmutex_unlock(&m);

    CHECK(m.acq_file  == NULL, "acq_file must be NULL after unlock");
    CHECK(m.acq_line  == 0,    "acq_line must be 0 after unlock");
    CHECK(m.owner_tid == -1,   "owner_tid must be -1 after unlock");
    END_TEST();
}

/* --- macro wrapper routes to _dbg ---------------------------------------- */
static void test_lock_debug_macro_wrapper_routes_to_dbg(void)
{
    BEGIN_TEST(lock_debug_kmutex_lock_macro_calls_dbg_variant);
    kmutex_t m;
    kmutex_init(&m);

    /* kmutex_lock() expands to kmutex_lock_dbg(m, __FILE__, __LINE__) */
    kmutex_lock(&m);

    CHECK(m.acq_file != NULL,  "macro wrapper must call _dbg and set acq_file");
    CHECK(m.acq_line >  0,     "macro wrapper must call _dbg and set acq_line > 0");

    kmutex_unlock(&m);
    END_TEST();
}

/* --- blocked-path TCB fields --------------------------------------------- */
static void test_lock_debug_mutex_blk_fields_on_block(void)
{
    BEGIN_TEST(lock_debug_mutex_dbg_sets_tcb_blk_fields_before_sched_block);

    kmutex_t m;
    kmutex_init(&m);

    /* Simulate the mutex being held by a different thread (TID 42). */
    m.owner_tid = 42;
    m.acq_file  = "other_thread.c";
    m.acq_line  = 77;

    /* Install hook: runs once during sched_yield() after sched_block().
     * It captures the TCB blk_* fields (still live at that moment) and
     * then releases the mutex so the next iteration acquires it. */
    _hook_mutex_ptr = &m;
    _hook_blk_time_us = 0u;
    mock_yield_hook = mutex_capture_and_release_hook;

    /* kmutex_lock_dbg will:
     *  1. Find mutex held → set TCB blk_* fields
     *  2. sched_block() → state = BLOCKED
     *  3. sched_yield() → hook fires: capture fields, release mutex
     *  4. Clear blk_time_us
     *  5. Loop back → mutex free → acquire → return */
    kmutex_lock_dbg(&m, "caller_file.c", 55);

    /* After successful acquire, verify what the hook captured. */
    CHECK(_hook_blk_time_us != 0u,
          "blk_time_us must be non-zero when thread is blocked");
    CHECK(_hook_blk_file != NULL && strcmp(_hook_blk_file, "caller_file.c") == 0,
          "blk_file must equal the call-site file passed to _dbg");
    CHECK(_hook_blk_line == 55,
          "blk_line must equal the call-site line passed to _dbg");
    CHECK(_hook_blk_what != NULL && strcmp(_hook_blk_what, "mutex") == 0,
          "blk_what must be \"mutex\" for a mutex block");
    CHECK(_hook_blk_holder_tid == 42,
          "blk_holder_tid must snapshot the holder TID at block time");

    /* After acquiring, blk_time_us must be cleared. */
    CHECK(CURRENT_TCB->blk_time_us == 0u,
          "blk_time_us must be 0 after the lock is acquired");

    /* The acq_* fields on the mutex must now reflect our acquisition. */
    CHECK(m.owner_tid == (int32_t)CURRENT_TCB->tid,
          "owner_tid must equal current TID after retry-acquire");
    CHECK(m.acq_file != NULL && strcmp(m.acq_file, "caller_file.c") == 0,
          "acq_file must be set on successful acquire");

    kmutex_unlock(&m);
    END_TEST();
}

/* --- spinlock timeout → deadlock panic ----------------------------------- */
static void test_lock_debug_spinlock_timeout_fires(void)
{
    BEGIN_TEST(lock_debug_spinlock_timeout_calls_deadlock_panic);

    spinlock_t s = {0};
    s.lock     = 1u;    /* pre-locked — simulate another holder */
    s.acq_tid  = 99;    /* holder TID */
    s.acq_file = "holder.c";
    s.acq_line = 123;

    mock_lock_panic_fired    = false;
    mock_lock_panic_wait_tid = 0;
    mock_lock_panic_hold_tid = 0;
    memset(mock_lock_panic_type, 0, sizeof(mock_lock_panic_type));

    if (setjmp(mock_lock_panic_jmp) == 0) {
        /* This spins until PICOOS_LOCK_TIMEOUT_MS elapses, then panics. */
        uint32_t saved = spinlock_irq_acquire(&s);
        /* Should never reach here. */
        spinlock_irq_release(&s, saved);
        CHECK(false, "spinlock_irq_acquire must have panicked before returning");
    } else {
        /* longjmp arrived — verify panic fired with the right information. */
        CHECK(mock_lock_panic_fired,
              "lock_deadlock_panic must have been called");
        CHECK(strcmp(mock_lock_panic_type, "spinlock") == 0,
              "panic lock_type must be \"spinlock\"");
        CHECK(mock_lock_panic_hold_tid == 99,
              "panic must report holder TID from s.acq_tid");
        CHECK(mock_lock_panic_wait_tid == (int32_t)CURRENT_TCB->tid,
              "panic must report current thread as the waiter");
    }

    /* Restore the spinlock so it doesn't interfere with subsequent tests. */
    s.lock    = 0u;
    s.acq_tid = -1;

    END_TEST();
}

#endif /* PICOOS_LOCK_DEBUG */

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("picoOS sync primitives — unit tests\n");
    printf("=====================================\n\n");

    /* Spinlock */
    test_spinlock_default_state();
    test_spinlock_acquire_sets_lock();
    test_spinlock_release_clears_lock();
    test_spinlock_irq_acquire_release();
    test_spinlock_reacquire_after_release();

    /* Mutex */
    test_mutex_init_state();
    test_mutex_lock_when_free();
    test_mutex_unlock();
    test_mutex_relock_after_unlock();

    /* Semaphore */
    test_semaphore_init_count();
    test_semaphore_signal_increments();
    test_semaphore_wait_decrements();
    test_semaphore_signal_wait_neutral();
    test_semaphore_multi_signal_multi_wait();

    /* Event flags */
    test_event_flags_init_zero();
    test_event_flags_set_individual_bits();
    test_event_flags_clear_bits();
    test_event_flags_set_does_not_clear_others();
    test_event_flags_all_32_bits();
    test_event_flags_wait_any_already_set();
    test_event_flags_wait_all_already_set();
    test_event_flags_wait_any_partial_match();

    /* Message queue */
    test_mqueue_init_state();
    test_mqueue_size_clamp();
    test_mqueue_send_recv_single();
    test_mqueue_fifo_order();
    test_mqueue_fill_to_capacity();
    test_mqueue_drain_to_empty();
    test_mqueue_msg_content_integrity();
    test_mqueue_interleaved_send_recv();

#ifdef PICOOS_LOCK_DEBUG
    printf("\nDeadlock detection (PICOOS_LOCK_DEBUG)\n");
    printf("----------------------------------------\n\n");

    /* Debug field initialisation */
    test_lock_debug_mutex_init_fields();
    test_lock_debug_semaphore_init_fields();

    /* Per-lock holder tracking */
    test_lock_debug_spinlock_acq_tid();
    test_lock_debug_mutex_acq_fields_on_lock();

    /* Macro wrapper verification */
    test_lock_debug_macro_wrapper_routes_to_dbg();

    /* Blocked-path TCB field recording */
    test_lock_debug_mutex_blk_fields_on_block();

    /* Spinlock timeout → panic (the primary deadlock detection test) */
    test_lock_debug_spinlock_timeout_fires();
#endif

    SUMMARY();
}
