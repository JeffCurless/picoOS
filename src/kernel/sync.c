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

#include "sync.h"
#include "sched.h"   /* sched_block, sched_unblock, sched_yield, current_tcb */
#include "arch.h"

#include <string.h>

/* =========================================================================
 * Spinlock
 * =========================================================================
 *
 * Two variants — see sync.h for the usage contract of each.
 *
 * spinlock_irq_acquire/release: saves interrupt state, disables IRQs while
 * the lock is held, and restores on release.  Required whenever the critical
 * section makes scheduler calls (sched_block, sched_remove_thread, etc.)
 * that must be atomic with respect to PendSV.
 *
 * spinlock_acquire/release: plain busy-wait with no interrupt manipulation.
 * Use only when the caller already controls interrupt state or the section
 * contains no scheduler calls.
 *
 * A production SMP spinlock would use LDREX/STREX; both versions here rely
 * on interrupt disable (or caller-guaranteed exclusion) for atomicity.
 * ========================================================================= */

uint32_t spinlock_irq_acquire(spinlock_t *s)
{
    while (1) {
        uint32_t save = save_and_disable_interrupts();
        if (s->lock == 0u) {
            s->lock = 1u;
            __dmb();
            return save;
        }
        restore_interrupts(save);
        __nop();
    }
}

void spinlock_irq_release(spinlock_t *s, uint32_t saved_irq)
{
    __dmb();
    s->lock = 0u;
    restore_interrupts(saved_irq);
}

void spinlock_acquire(spinlock_t *s)
{
    while (1) {
        __disable_irq();
        if (s->lock == 0u) {
            s->lock = 1u;
            __dmb();
            __enable_irq();
            return;
        }
        __enable_irq();
        __nop();
    }
}

void spinlock_release(spinlock_t *s)
{
    __dmb();
    s->lock = 0u;
}

/* =========================================================================
 * Internal helpers
 * =========================================================================
 *
 * Waiter queues are singly-linked lists through tcb_t.next.
 * These helpers must be called with the relevant spinlock already held.
 * ========================================================================= */

/* waiter_enqueue — append t to the tail of the waiter list rooted at *head.
 *
 * Waiters are kept in FIFO order so that the first thread to block is the
 * first to be woken (fair scheduling).  The list is singly-linked through
 * tcb_t.next.  Must be called with the owning spinlock already held. */
static void waiter_enqueue(tcb_t **head, tcb_t *t)
{
    t->next = NULL;
    if (*head == NULL) {
        *head = t;
        return;
    }
    tcb_t *cur = *head;
    while (cur->next != NULL) {
        cur = cur->next;
    }
    cur->next = t;
}

/* waiter_dequeue — remove and return the head of the waiter list.
 *
 * Returns NULL if the list is empty.  The returned TCB's next pointer is
 * cleared so it does not dangle.  Must be called with the owning spinlock
 * already held. */
static tcb_t *waiter_dequeue(tcb_t **head)
{
    if (*head == NULL) {
        return NULL;
    }
    tcb_t *t = *head;
    *head = t->next;
    t->next = NULL;
    return t;
}

/* =========================================================================
 * Mutex
 * ========================================================================= */

void kmutex_init(kmutex_t *m)
{
    m->spin.lock  = 0u;
    m->owner_tid  = -1;
    m->count      = 0u;
    m->waiters    = NULL;
}

void kmutex_lock(kmutex_t *m)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&m->spin);

        if (m->owner_tid == -1) {
            /* Mutex is free — claim it. */
            m->owner_tid = (int32_t)current_tcb->tid;
            m->count     = 1u;
            spinlock_irq_release(&m->spin, irq_save);
            return;
        }

        /* Mutex is held by another thread — block and yield. */
        waiter_enqueue(&m->waiters, (tcb_t *)current_tcb);
        sched_block((tcb_t *)current_tcb);
        spinlock_irq_release(&m->spin, irq_save);

        /* When we are unblocked the mutex may still be held; loop. */
        sched_yield();
    }
}

void kmutex_unlock(kmutex_t *m)
{
    uint32_t irq_save = spinlock_irq_acquire(&m->spin);

    m->owner_tid = -1;
    m->count     = 0u;

    /* Wake the first waiting thread (FIFO policy). */
    tcb_t *next = waiter_dequeue(&m->waiters);
    if (next != NULL) {
        sched_unblock(next);
    }

    spinlock_irq_release(&m->spin, irq_save);
}

/* =========================================================================
 * Semaphore
 * ========================================================================= */

void ksemaphore_init(ksemaphore_t *s, int32_t initial_count)
{
    s->spin.lock = 0u;
    s->count     = initial_count;
    s->waiters   = NULL;
}

void ksemaphore_wait(ksemaphore_t *s)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&s->spin);

        if (s->count > 0) {
            s->count--;
            spinlock_irq_release(&s->spin, irq_save);
            return;
        }

        /* Count is 0 — block without touching count. */
        waiter_enqueue(&s->waiters, (tcb_t *)current_tcb);
        sched_block((tcb_t *)current_tcb);
        spinlock_irq_release(&s->spin, irq_save);

        sched_yield();
    }
}

void ksemaphore_signal(ksemaphore_t *s)
{
    uint32_t irq_save = spinlock_irq_acquire(&s->spin);

    s->count++;

    /* Always try to wake a waiter; count > 0 ensures it will succeed. */
    tcb_t *t = waiter_dequeue(&s->waiters);
    if (t != NULL) {
        sched_unblock(t);
    }

    spinlock_irq_release(&s->spin, irq_save);
}

/* =========================================================================
 * Event flags
 * ========================================================================= */

/* Per-waiter context: we need to know what mask each waiter is waiting on
 * and whether it wants all or any bits.  We stash this in a small parallel
 * array indexed by the waiter's position in the queue.
 *
 * Teaching simplification: since MAX_THREADS is small (16), a linear scan
 * over all waiters is perfectly acceptable.
 */
#define MAX_EVENT_WAITERS MAX_THREADS

typedef struct {
    tcb_t    *thread;
    uint32_t  mask;
    bool      wait_for_all;
} event_waiter_t;

static event_waiter_t event_waiter_pool[MAX_EVENT_WAITERS];

/* event_waiter_alloc — reserve a slot in the event waiter pool for thread t.
 *
 * Stores the wait mask and the wait-for-all flag alongside the TCB pointer so
 * that event_flags_set() can later check whether each blocked thread's
 * condition has been met.  Returns the pool index on success, or -1 if the
 * pool is full (which cannot happen in normal operation since the pool is
 * sized to MAX_THREADS). */
static int event_waiter_alloc(tcb_t *t, uint32_t mask, bool wait_for_all)
{
    for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
        if (event_waiter_pool[i].thread == NULL) {
            event_waiter_pool[i].thread       = t;
            event_waiter_pool[i].mask         = mask;
            event_waiter_pool[i].wait_for_all = wait_for_all;
            return i;
        }
    }
    return -1;
}

/* event_waiter_free — release the pool slot held by thread t.
 *
 * Called after a thread's wait condition is satisfied (inside
 * event_flags_set()) or when a thread is forcibly unblocked.  Clears the
 * thread pointer so the slot becomes available for future waiters. */
static void event_waiter_free(tcb_t *t)
{
    for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
        if (event_waiter_pool[i].thread == t) {
            event_waiter_pool[i].thread = NULL;
            return;
        }
    }
}

void event_flags_init(event_flags_t *e)
{
    e->spin.lock = 0u;
    e->flags     = 0u;
    e->waiters   = NULL;
}

void event_flags_set(event_flags_t *e, uint32_t mask)
{
    uint32_t irq_save = spinlock_irq_acquire(&e->spin);

    e->flags |= mask;

    /* Wake all threads whose wait condition is now satisfied. */
    tcb_t **prev_ptr = &e->waiters;
    tcb_t  *cur      = e->waiters;

    while (cur != NULL) {
        tcb_t *next = cur->next;

        /* Find this thread's wait parameters. */
        bool   satisfied    = false;
        bool   wait_for_all = false;
        uint32_t wait_mask  = 0u;

        for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
            if (event_waiter_pool[i].thread == cur) {
                wait_mask    = event_waiter_pool[i].mask;
                wait_for_all = event_waiter_pool[i].wait_for_all;
                break;
            }
        }

        if (wait_for_all) {
            satisfied = ((e->flags & wait_mask) == wait_mask);
        } else {
            satisfied = ((e->flags & wait_mask) != 0u);
        }

        if (satisfied) {
            /* Remove from list. */
            *prev_ptr = next;
            cur->next = NULL;
            event_waiter_free(cur);
            sched_unblock(cur);
        } else {
            prev_ptr = &cur->next;
        }

        cur = next;
    }

    spinlock_irq_release(&e->spin, irq_save);
}

void event_flags_clear(event_flags_t *e, uint32_t mask)
{
    uint32_t irq_save = spinlock_irq_acquire(&e->spin);
    e->flags &= ~mask;
    spinlock_irq_release(&e->spin, irq_save);
}

uint32_t event_flags_wait(event_flags_t *e, uint32_t mask, bool wait_for_all)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&e->spin);

        bool satisfied;
        if (wait_for_all) {
            satisfied = ((e->flags & mask) == mask);
        } else {
            satisfied = ((e->flags & mask) != 0u);
        }

        if (satisfied) {
            uint32_t result = e->flags;
            spinlock_irq_release(&e->spin, irq_save);
            return result;
        }

        /* Not yet satisfied — block this thread. */
        event_waiter_alloc((tcb_t *)current_tcb, mask, wait_for_all);
        waiter_enqueue(&e->waiters, (tcb_t *)current_tcb);
        sched_block((tcb_t *)current_tcb);
        spinlock_irq_release(&e->spin, irq_save);

        sched_yield();
        /* Loop back to re-check in case of spurious wakeup. */
    }
}

/* =========================================================================
 * Message queue
 * ========================================================================= */

void mqueue_init(mqueue_t *q, uint32_t msg_size)
{
    q->spin.lock    = 0u;
    q->msg_size     = (msg_size <= MQ_MSG_SIZE) ? msg_size : MQ_MSG_SIZE;
    q->head         = 0u;
    q->tail         = 0u;
    q->count        = 0u;
    q->send_waiters = NULL;
    q->recv_waiters = NULL;
    memset(q->buf, 0, sizeof(q->buf));
}

void mqueue_send(mqueue_t *q, const void *msg)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&q->spin);

        if (q->count < MQ_MAX_MSG) {
            /* Space available — copy message into ring buffer. */
            memcpy(q->buf[q->tail], msg, q->msg_size);
            q->tail = (q->tail + 1u) % MQ_MAX_MSG;
            q->count++;

            /* Wake a waiting receiver if any. */
            tcb_t *receiver = waiter_dequeue(&q->recv_waiters);
            if (receiver != NULL) {
                sched_unblock(receiver);
            }

            spinlock_irq_release(&q->spin, irq_save);
            return;
        }

        /* Queue full — block sender. */
        waiter_enqueue(&q->send_waiters, (tcb_t *)current_tcb);
        sched_block((tcb_t *)current_tcb);
        spinlock_irq_release(&q->spin, irq_save);

        sched_yield();
    }
}

void mqueue_recv(mqueue_t *q, void *msg_out)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&q->spin);

        if (q->count > 0u) {
            /* Data available — copy out of ring buffer. */
            memcpy(msg_out, q->buf[q->head], q->msg_size);
            q->head = (q->head + 1u) % MQ_MAX_MSG;
            q->count--;

            /* Wake a waiting sender if any. */
            tcb_t *sender = waiter_dequeue(&q->send_waiters);
            if (sender != NULL) {
                sched_unblock(sender);
            }

            spinlock_irq_release(&q->spin, irq_save);
            return;
        }

        /* Queue empty — block receiver. */
        waiter_enqueue(&q->recv_waiters, (tcb_t *)current_tcb);
        sched_block((tcb_t *)current_tcb);
        spinlock_irq_release(&q->spin, irq_save);

        sched_yield();
    }
}
