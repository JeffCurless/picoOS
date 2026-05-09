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

#ifndef KERNEL_SYNC_H
#define KERNEL_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "task.h"

/* -------------------------------------------------------------------------
 * Spinlock
 *
 * Two variants are provided:
 *
 *   spinlock_irq_acquire / spinlock_irq_release
 *     Saves the current interrupt state, disables interrupts, and acquires
 *     the lock.  The saved state is returned and must be passed back to
 *     spinlock_irq_release so the interrupt state is correctly restored.
 *     Use this variant whenever the critical section calls scheduler
 *     primitives (sched_block, sched_yield, etc.) that must not be
 *     preempted by PendSV between a state write and a queue operation.
 *
 *   spinlock_acquire / spinlock_release
 *     Acquires and releases the lock without touching interrupt state.
 *     Use only when the caller already manages interrupts (e.g. IRQs are
 *     already disabled) or the critical section contains no scheduler calls.
 *
 * Teaching note: a production SMP spinlock would use LDREX/STREX for the
 * test-and-set; here we rely on interrupt disable for atomicity, which is
 * sufficient for the RP2040 teaching workload.
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t lock;   /* 0 = free, 1 = held */
} spinlock_t;

/* IRQ-aware pair — saves and restores interrupt enable state. */
uint32_t spinlock_irq_acquire(spinlock_t *s);
void     spinlock_irq_release(spinlock_t *s, uint32_t saved_irq);

/* Plain pair — does not touch interrupt state. */
void spinlock_acquire(spinlock_t *s);
void spinlock_release(spinlock_t *s);

/* -------------------------------------------------------------------------
 * Mutex
 *
 * Non-recursive.  Threads that cannot acquire the mutex are moved to
 * BLOCKED state and placed on the waiter queue.
 * ------------------------------------------------------------------------- */
typedef struct {
    spinlock_t       spin;
    volatile int32_t owner_tid;   /* TID of the holding thread, -1 = free  */
    volatile uint32_t count;      /* lock depth (always 0 or 1 here)       */
    tcb_t           *waiters;     /* head of blocked-thread list            */
} kmutex_t;

void kmutex_init(kmutex_t *m);
void kmutex_lock(kmutex_t *m);
void kmutex_unlock(kmutex_t *m);

/* -------------------------------------------------------------------------
 * Semaphore
 *
 * Counting semaphore.  count < 0 means that (-count) threads are waiting.
 * ------------------------------------------------------------------------- */
typedef struct {
    spinlock_t       spin;
    volatile int32_t count;    /* current count; negative = threads waiting */
    tcb_t           *waiters;  /* head of blocked-thread list               */
} ksemaphore_t;

void ksemaphore_init(ksemaphore_t *s, int32_t initial_count);
void ksemaphore_wait(ksemaphore_t *s);    /* P() / down()  */
void ksemaphore_signal(ksemaphore_t *s);  /* V() / up()    */

/* -------------------------------------------------------------------------
 * Event flags
 *
 * A set of 32 independent binary flags.  Threads can wait for any subset
 * of flags (wait_for_all = false) or for all flags in a mask to be set
 * (wait_for_all = true).
 * ------------------------------------------------------------------------- */
typedef struct {
    spinlock_t       spin;
    volatile uint32_t flags;   /* bitmask of currently set flags */
    tcb_t           *waiters;  /* head of blocked-thread list    */
} event_flags_t;

void event_flags_init(event_flags_t *e);
void event_flags_set(event_flags_t *e, uint32_t mask);
void event_flags_clear(event_flags_t *e, uint32_t mask);

/*
 * event_flags_wait — block until (flags & mask) satisfies the condition.
 *   wait_for_all = true  : ALL bits in mask must be set before waking.
 *   wait_for_all = false : ANY bit in mask is sufficient to wake.
 * Returns the flags value at the time the thread was woken.
 */
uint32_t event_flags_wait(event_flags_t *e, uint32_t mask, bool wait_for_all);

/* -------------------------------------------------------------------------
 * Message queue
 *
 * Fixed-depth, fixed-width ring buffer.  Senders block when full; receivers
 * block when empty.  msg_size must be <= MQ_MSG_SIZE.
 * ------------------------------------------------------------------------- */
#define MQ_MAX_MSG  16u
#define MQ_MSG_SIZE 64u

typedef struct {
    spinlock_t       spin;
    uint8_t          buf[MQ_MAX_MSG][MQ_MSG_SIZE];
    uint32_t         msg_size;      /* size of each message in bytes        */
    uint32_t         head;          /* index of next message to read        */
    uint32_t         tail;          /* index of next slot to write          */
    uint32_t         count;         /* number of messages currently queued  */
    tcb_t           *send_waiters;  /* threads blocked on send (queue full) */
    tcb_t           *recv_waiters;  /* threads blocked on recv (queue empty)*/
} mqueue_t;

void mqueue_init(mqueue_t *q, uint32_t msg_size);
void mqueue_send(mqueue_t *q, const void *msg);
void mqueue_recv(mqueue_t *q, void *msg_out);

#endif /* KERNEL_SYNC_H */
