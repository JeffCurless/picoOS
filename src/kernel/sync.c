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
#include "sched.h"   /* sched_block, sched_unblock, sched_yield, CURRENT_TCB */
#include "arch.h"

#include <string.h>

/* =========================================================================
 * Module-level sync_init
 *
 * Initialises module-level spinlocks that must be claimed before any
 * sync primitive can be used from more than one core simultaneously.
 * Call once from main() before sched_start().
 * ========================================================================= */

/* Protects event_waiter_pool[] across all event_flags_t objects.
 * Without this, concurrent event_flags_set() calls on different objects
 * from two cores can race on the shared pool. */
static spinlock_t event_pool_lock = {0};

void sync_init(void)
{
    spinlock_init(&event_pool_lock);
}

/* Prevent the PICOOS_LOCK_DEBUG macro wrappers declared in sync.h from
 * expanding function-definition tokens below.  External callers still use
 * the macro versions (via their own #include "sync.h"). */
#ifdef PICOOS_LOCK_DEBUG
#undef kmutex_lock
#undef ksemaphore_wait
#undef event_flags_wait
#undef mqueue_send
#undef mqueue_recv
#endif

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

/* spinlock_init — claim a free RP2040 hardware spinlock for SMP-safe use.
 * Must be called once before the spinlock is first acquired on more than one
 * core.  The {0} default leaves hw = NULL, which causes the IRQ-disable-only
 * fallback to be used (safe during single-core init before sched_start). */
void spinlock_init(spinlock_t *s)
{
    s->hw   = spin_lock_init(spin_lock_claim_unused(true));
    s->lock = 0u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_file = NULL;
    s->acq_line = 0;
    s->acq_tid  = -1;
#endif
}

uint32_t spinlock_irq_acquire(spinlock_t *s)
{
    if (s->hw != NULL) {
        /* SMP-safe: RP2040 hardware spinlock disables IRQs and spins until
         * the lock register is available on both cores atomically. */
#ifdef PICOOS_LOCK_DEBUG
        uint32_t save = spin_lock_blocking(s->hw);
        s->acq_tid = CURRENT_TCB ? (int32_t)CURRENT_TCB->tid : -1;
        return save;
#else
        return spin_lock_blocking(s->hw);
#endif
    }

    /* Fallback (single-core init phase, hw not yet claimed): IRQ-disable only.
     * Safe because Core 1 is not yet scheduling when this path is taken. */
#ifdef PICOOS_LOCK_DEBUG
    uint64_t deadline = time_us_64() + (uint64_t)PICOOS_LOCK_TIMEOUT_MS * 1000u;
#endif
    uint32_t save = save_and_disable_interrupts();
    while (s->lock != 0u) {
        restore_interrupts(save);
        save = save_and_disable_interrupts();
#ifdef PICOOS_LOCK_DEBUG
        if (time_us_64() > deadline) {
            lock_deadlock_panic("spinlock",
                "(spin-wait)", 0,
                CURRENT_TCB ? (uint32_t)CURRENT_TCB->tid : 0u,
                CURRENT_TCB ? CURRENT_TCB->name : "?",
                s->acq_file, s->acq_line, s->acq_tid);
        }
#endif
    }
    s->lock = 1u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_tid = CURRENT_TCB ? (int32_t)CURRENT_TCB->tid : -1;
#endif
    return save;
}

void spinlock_irq_release(spinlock_t *s, uint32_t saved_irq)
{
    if (s->hw != NULL) {
#ifdef PICOOS_LOCK_DEBUG
        s->acq_tid  = -1;
        s->acq_file = NULL;
        s->acq_line = 0;
#endif
        spin_unlock(s->hw, saved_irq);
        return;
    }

    /* Fallback (single-core init phase). */
    s->lock = 0u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_tid  = -1;
    s->acq_file = NULL;
    s->acq_line = 0;
#endif
    restore_interrupts(saved_irq);
}

void spinlock_acquire(spinlock_t *s)
{
    if (s->hw != NULL) {
        /* Spin-read the HW spinlock register without touching IRQ state.
         * The register returns non-zero when successfully claimed. */
        while (!*s->hw) { /* busy-wait */ }
        __dmb();
#ifdef PICOOS_LOCK_DEBUG
        s->acq_tid = CURRENT_TCB ? (int32_t)CURRENT_TCB->tid : -1;
#endif
        return;
    }

    /* Fallback: software spin (single-core only). */
    while (s->lock != 0u) { /* spin */ }
    s->lock = 1u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_tid = CURRENT_TCB ? (int32_t)CURRENT_TCB->tid : -1;
#endif
}

void spinlock_release(spinlock_t *s)
{
    if (s->hw != NULL) {
#ifdef PICOOS_LOCK_DEBUG
        s->acq_tid  = -1;
        s->acq_file = NULL;
        s->acq_line = 0;
#endif
        spin_unlock_unsafe(s->hw);
        return;
    }

    s->lock = 0u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_tid  = -1;
    s->acq_file = NULL;
    s->acq_line = 0;
#endif
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
    spinlock_init(&m->spin);  /* claim an RP2040 HW spinlock for SMP safety */
    m->owner_tid  = -1;
    m->count      = 0u;
    m->waiters    = NULL;
#ifdef PICOOS_LOCK_DEBUG
    m->acq_file      = NULL;
    m->acq_line      = 0;
    m->acq_time_us   = 0u;
#endif
}

void kmutex_lock(kmutex_t *m)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&m->spin);

        if (m->owner_tid == -1) {
            /* Mutex is free — claim it. */
            m->owner_tid = (int32_t)CURRENT_TCB->tid;
            m->count     = 1u;
            spinlock_irq_release(&m->spin, irq_save);
            return;
        }

        /* Mutex is held by another thread — block and yield. */
        waiter_enqueue(&m->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
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
#ifdef PICOOS_LOCK_DEBUG
    m->acq_file    = NULL;
    m->acq_line    = 0;
    m->acq_time_us = 0u;
#endif

    /* Wake the first waiting thread (FIFO policy). */
    tcb_t *next = waiter_dequeue(&m->waiters);
    if (next != NULL) {
        sched_unblock(next);
    }

    spinlock_irq_release(&m->spin, irq_save);
}

#ifdef PICOOS_LOCK_DEBUG
void kmutex_lock_dbg(kmutex_t *m, const char *file, int line)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&m->spin);

        if (m->owner_tid == -1) {
            m->owner_tid   = (int32_t)CURRENT_TCB->tid;
            m->count       = 1u;
            m->acq_file    = file;
            m->acq_line    = line;
            m->acq_time_us = time_us_64();
            /* Record location on the spinlock for spinlock-level diagnostics. */
            m->spin.acq_file = file;
            m->spin.acq_line = line;
            spinlock_irq_release(&m->spin, irq_save);
            return;
        }

        /* Record blocking info on the TCB before yielding. */
        ((tcb_t *)CURRENT_TCB)->blk_time_us     = time_us_64();
        ((tcb_t *)CURRENT_TCB)->blk_file        = file;
        ((tcb_t *)CURRENT_TCB)->blk_line        = line;
        ((tcb_t *)CURRENT_TCB)->blk_what        = "mutex";
        ((tcb_t *)CURRENT_TCB)->blk_holder_tid  = m->owner_tid;
        ((tcb_t *)CURRENT_TCB)->blk_holder_file = m->acq_file;
        ((tcb_t *)CURRENT_TCB)->blk_holder_line = m->acq_line;

        waiter_enqueue(&m->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&m->spin, irq_save);
        sched_yield();

        /* Unblocked — clear block marker and loop to re-check. */
        ((tcb_t *)CURRENT_TCB)->blk_time_us = 0u;
    }
}
#endif

/* =========================================================================
 * Semaphore
 * ========================================================================= */

void ksemaphore_init(ksemaphore_t *s, int32_t initial_count)
{
    spinlock_init(&s->spin);   /* claim RP2040 HW spinlock for SMP safety */
    s->count   = initial_count;
    s->waiters = NULL;
#ifdef PICOOS_LOCK_DEBUG
    s->spin.acq_file = NULL;
    s->spin.acq_line = 0;
    s->spin.acq_tid  = -1;
#endif
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
        waiter_enqueue(&s->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
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

#ifdef PICOOS_LOCK_DEBUG
void ksemaphore_wait_dbg(ksemaphore_t *s, const char *file, int line)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&s->spin);

        if (s->count > 0) {
            s->count--;
            spinlock_irq_release(&s->spin, irq_save);
            return;
        }

        ((tcb_t *)CURRENT_TCB)->blk_time_us     = time_us_64();
        ((tcb_t *)CURRENT_TCB)->blk_file        = file;
        ((tcb_t *)CURRENT_TCB)->blk_line        = line;
        ((tcb_t *)CURRENT_TCB)->blk_what        = "semaphore";
        ((tcb_t *)CURRENT_TCB)->blk_holder_tid  = -1;
        ((tcb_t *)CURRENT_TCB)->blk_holder_file = NULL;
        ((tcb_t *)CURRENT_TCB)->blk_holder_line = 0;

        waiter_enqueue(&s->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&s->spin, irq_save);
        sched_yield();

        ((tcb_t *)CURRENT_TCB)->blk_time_us = 0u;
    }
}
#endif

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
 * event_pool_lock protects event_waiter_pool[] across all event_flags_t
 * objects.  Without it, two cores holding different e->spin locks could race
 * on the pool simultaneously.  IRQs are already disabled by the caller's
 * spinlock_irq_acquire(&e->spin), so the nested acquire is a no-op for IRQ
 * state and only serves to claim the HW spinlock.
 *
 * Returns the pool index on success, or -1 if the pool is full. */
static int event_waiter_alloc(tcb_t *t, uint32_t mask, bool wait_for_all)
{
    uint32_t save = spinlock_irq_acquire(&event_pool_lock);
    int idx = -1;
    for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
        if (event_waiter_pool[i].thread == NULL) {
            event_waiter_pool[i].thread       = t;
            event_waiter_pool[i].mask         = mask;
            event_waiter_pool[i].wait_for_all = wait_for_all;
            idx = i;
            break;
        }
    }
    spinlock_irq_release(&event_pool_lock, save);
    return idx;
}

/* event_waiter_free — release the pool slot held by thread t. */
static void event_waiter_free(tcb_t *t)
{
    uint32_t save = spinlock_irq_acquire(&event_pool_lock);
    for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
        if (event_waiter_pool[i].thread == t) {
            event_waiter_pool[i].thread = NULL;
            break;
        }
    }
    spinlock_irq_release(&event_pool_lock, save);
}

void event_flags_init(event_flags_t *e)
{
    spinlock_init(&e->spin);   /* claim RP2040 HW spinlock for SMP safety */
    e->flags   = 0u;
    e->waiters = NULL;
#ifdef PICOOS_LOCK_DEBUG
    e->spin.acq_file = NULL;
    e->spin.acq_line = 0;
    e->spin.acq_tid  = -1;
#endif
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

        /* Find this thread's wait parameters under the pool lock. */
        bool   satisfied    = false;
        bool   wait_for_all = false;
        uint32_t wait_mask  = 0u;

        {
            uint32_t pool_save = spinlock_irq_acquire(&event_pool_lock);
            for (int i = 0; i < MAX_EVENT_WAITERS; i++) {
                if (event_waiter_pool[i].thread == cur) {
                    wait_mask    = event_waiter_pool[i].mask;
                    wait_for_all = event_waiter_pool[i].wait_for_all;
                    break;
                }
            }
            spinlock_irq_release(&event_pool_lock, pool_save);
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
        event_waiter_alloc((tcb_t *)CURRENT_TCB, mask, wait_for_all);
        waiter_enqueue(&e->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&e->spin, irq_save);

        sched_yield();
        /* Loop back to re-check in case of spurious wakeup. */
    }
}

#ifdef PICOOS_LOCK_DEBUG
uint32_t event_flags_wait_dbg(event_flags_t *e, uint32_t mask, bool wait_for_all,
                               const char *file, int line)
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

        ((tcb_t *)CURRENT_TCB)->blk_time_us     = time_us_64();
        ((tcb_t *)CURRENT_TCB)->blk_file        = file;
        ((tcb_t *)CURRENT_TCB)->blk_line        = line;
        ((tcb_t *)CURRENT_TCB)->blk_what        = "event_flags";
        ((tcb_t *)CURRENT_TCB)->blk_holder_tid  = -1;
        ((tcb_t *)CURRENT_TCB)->blk_holder_file = NULL;
        ((tcb_t *)CURRENT_TCB)->blk_holder_line = 0;

        event_waiter_alloc((tcb_t *)CURRENT_TCB, mask, wait_for_all);
        waiter_enqueue(&e->waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&e->spin, irq_save);
        sched_yield();

        ((tcb_t *)CURRENT_TCB)->blk_time_us = 0u;
    }
}
#endif

/* =========================================================================
 * Message queue
 * ========================================================================= */

void mqueue_init(mqueue_t *q, uint32_t msg_size)
{
    spinlock_init(&q->spin);   /* claim RP2040 HW spinlock for SMP safety */
    q->msg_size     = (msg_size <= MQ_MSG_SIZE) ? msg_size : MQ_MSG_SIZE;
    q->head         = 0u;
    q->tail         = 0u;
    q->count        = 0u;
    q->send_waiters = NULL;
    q->recv_waiters = NULL;
    memset(q->buf, 0, sizeof(q->buf));
#ifdef PICOOS_LOCK_DEBUG
    q->spin.acq_file = NULL;
    q->spin.acq_line = 0;
    q->spin.acq_tid  = -1;
#endif
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
        waiter_enqueue(&q->send_waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
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
        waiter_enqueue(&q->recv_waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&q->spin, irq_save);

        sched_yield();
    }
}

bool mqueue_try_send(mqueue_t *q, const void *msg)
{
    uint32_t irq_save = spinlock_irq_acquire(&q->spin);

    if (q->count >= MQ_MAX_MSG) {
        spinlock_irq_release(&q->spin, irq_save);
        return false;
    }

    memcpy(q->buf[q->tail], msg, q->msg_size);
    q->tail = (q->tail + 1u) % MQ_MAX_MSG;
    q->count++;

    tcb_t *receiver = waiter_dequeue(&q->recv_waiters);
    if (receiver != NULL) {
        sched_unblock(receiver);
    }

    spinlock_irq_release(&q->spin, irq_save);
    return true;
}

bool mqueue_try_recv(mqueue_t *q, void *msg_out)
{
    uint32_t irq_save = spinlock_irq_acquire(&q->spin);

    if (q->count == 0u) {
        spinlock_irq_release(&q->spin, irq_save);
        return false;
    }

    memcpy(msg_out, q->buf[q->head], q->msg_size);
    q->head = (q->head + 1u) % MQ_MAX_MSG;
    q->count--;

    tcb_t *sender = waiter_dequeue(&q->send_waiters);
    if (sender != NULL) {
        sched_unblock(sender);
    }

    spinlock_irq_release(&q->spin, irq_save);
    return true;
}

#ifdef PICOOS_LOCK_DEBUG
void mqueue_send_dbg(mqueue_t *q, const void *msg, const char *file, int line)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&q->spin);

        if (q->count < MQ_MAX_MSG) {
            memcpy(q->buf[q->tail], msg, q->msg_size);
            q->tail = (q->tail + 1u) % MQ_MAX_MSG;
            q->count++;

            tcb_t *receiver = waiter_dequeue(&q->recv_waiters);
            if (receiver != NULL) {
                sched_unblock(receiver);
            }

            spinlock_irq_release(&q->spin, irq_save);
            return;
        }

        ((tcb_t *)CURRENT_TCB)->blk_time_us     = time_us_64();
        ((tcb_t *)CURRENT_TCB)->blk_file        = file;
        ((tcb_t *)CURRENT_TCB)->blk_line        = line;
        ((tcb_t *)CURRENT_TCB)->blk_what        = "mqueue_send";
        ((tcb_t *)CURRENT_TCB)->blk_holder_tid  = -1;
        ((tcb_t *)CURRENT_TCB)->blk_holder_file = NULL;
        ((tcb_t *)CURRENT_TCB)->blk_holder_line = 0;

        waiter_enqueue(&q->send_waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&q->spin, irq_save);
        sched_yield();

        ((tcb_t *)CURRENT_TCB)->blk_time_us = 0u;
    }
}

void mqueue_recv_dbg(mqueue_t *q, void *msg_out, const char *file, int line)
{
    while (1) {
        uint32_t irq_save = spinlock_irq_acquire(&q->spin);

        if (q->count > 0u) {
            memcpy(msg_out, q->buf[q->head], q->msg_size);
            q->head = (q->head + 1u) % MQ_MAX_MSG;
            q->count--;

            tcb_t *sender = waiter_dequeue(&q->send_waiters);
            if (sender != NULL) {
                sched_unblock(sender);
            }

            spinlock_irq_release(&q->spin, irq_save);
            return;
        }

        ((tcb_t *)CURRENT_TCB)->blk_time_us     = time_us_64();
        ((tcb_t *)CURRENT_TCB)->blk_file        = file;
        ((tcb_t *)CURRENT_TCB)->blk_line        = line;
        ((tcb_t *)CURRENT_TCB)->blk_what        = "mqueue_recv";
        ((tcb_t *)CURRENT_TCB)->blk_holder_tid  = -1;
        ((tcb_t *)CURRENT_TCB)->blk_holder_file = NULL;
        ((tcb_t *)CURRENT_TCB)->blk_holder_line = 0;

        waiter_enqueue(&q->recv_waiters, (tcb_t *)CURRENT_TCB);
        sched_block((tcb_t *)CURRENT_TCB);
        spinlock_irq_release(&q->spin, irq_save);
        sched_yield();

        ((tcb_t *)CURRENT_TCB)->blk_time_us = 0u;
    }
}
#endif /* PICOOS_LOCK_DEBUG */
