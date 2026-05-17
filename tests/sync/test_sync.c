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
    CHECK(m.owner_tid == (int32_t)current_tcb->tid,
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
    CHECK(m.owner_tid == (int32_t)current_tcb->tid,
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

    SUMMARY();
}
