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

/* -------------------------------------------------------------------------
 * HW spinlock budget
 *
 * The RP2040 has 32 HW spinlock registers, but only 8 of them — IDs 24-31,
 * the SDK's "claim-free" range — are reachable via spin_lock_claim_unused(),
 * which is what spinlock_init() below calls.  IDs 0-13 are reserved for the
 * SDK's own use; 16-23 are a separate "striped" pool that the SDK shares
 * internally for mutex_init()/sem_init()/critical_section_init() (and hence
 * for lwIP, BTstack, and async_context) — designed to tolerate contention,
 * so it never exhausts.  The 8-slot claim-free range is the one that can.
 *
 * A prior revision gave every ksemaphore_t / event_flags_t / mqueue_t its own
 * HW spinlock; creating more than a handful would make spin_lock_claim_unused
 * (true) panic and halt the system silently (commit 9a4ac10, fixed by
 * aa38e26).  That fix introduced one HW spinlock shared by those three types,
 * but kmutex_t — the one primitive an application can freely instantiate,
 * since "process" is a software abstraction with no MPU enforcement — still
 * claimed one HW spinlock per instance.
 *
 * This revision generalises the fix: all four primitive types now draw their
 * HW spinlock from prim_pool[] below, a small fixed-size striping pool — the
 * same technique the SDK uses for its own internal striped range.  Spreading
 * objects across PRIM_POOL_SIZE stripes (rather than funnelling everything
 * through one shared lock) keeps cross-object contention low while bounding
 * total HW spinlock consumption to a constant, no matter how many software
 * locks the OS or an application creates.
 *
 * The pool's first two stripes claim PICO_SPINLOCK_ID_OS1 and _OS2 (IDs 14,
 * 15) directly instead of going through spin_lock_claim_unused().  The SDK
 * documents these as "reserved for exclusive use by an operating system...
 * co-existing with the SDK," and a full-tree search confirms nothing in the
 * SDK, CYW43, lwIP, or BTstack ever claims them — two HW spinlocks for free,
 * without touching the scarce 8-slot claim-free range at all.
 *
 * picoOS's total HW spinlock claims, with this revision:
 *   event_pool_lock, heap_lock (mem.c), sched_lock (sched.c) — one each,
 *     dedicated, because they sit on the hottest paths in the kernel (every
 *     kmalloc/kfree, every context switch); sharing a stripe with unrelated
 *     objects there would be a real throughput loss.
 *   prim_pool[0..PRIM_POOL_SIZE-1] — shared by kmutex_t, ksemaphore_t,
 *     event_flags_t, and mqueue_t via prim_pool_assign().
 * That's (3 + PRIM_POOL_SIZE) HW spinlocks total, of which only
 * (1 + PRIM_POOL_SIZE) come from the contended claim-free range (event_pool_
 * lock, heap_lock, sched_lock, plus all but the first two pool stripes) —
 * see sync_spinlock_report() for a runtime view of how much headroom remains.
 * ------------------------------------------------------------------------- */
#define PRIM_POOL_SIZE 4

/* Protects event_waiter_pool[] across all event_flags_t objects.
 * Without this, concurrent event_flags_set() calls on different objects
 * from two cores can race on the shared pool. */
static spinlock_t event_pool_lock = {0};

/* Shared striping pool — see "HW spinlock budget" above.  Each entry wraps
 * exactly one RP2040 HW spinlock; kmutex_t / ksemaphore_t / event_flags_t /
 * mqueue_t alias their spin.hw to one of these via prim_pool_assign() instead
 * of claiming a HW spinlock of their own. */
static spinlock_t prim_pool[PRIM_POOL_SIZE];

/* claim_spinlock_by_id — claim a *specific* RP2040 HW spinlock by ID instead
 * of letting the SDK pick one from the contended claim-free range (24-31).
 *
 * Used only for the OS1/OS2 IDs that the SDK carves out for OS use and never
 * touches itself.  spin_lock_claim() registers the claim in the SDK's claim
 * bitmask purely for hygiene — spin_lock_claim_unused() never selects IDs
 * outside 24-31 regardless, but registering the claim makes any future
 * accidental double-claim of these IDs assert loudly instead of corrupting
 * shared state quietly. */
static void claim_spinlock_by_id(spinlock_t *s, uint lock_num)
{
    spin_lock_claim(lock_num);
    s->hw   = spin_lock_init(lock_num);
    s->lock = 0u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_file = NULL;
    s->acq_line = 0;
    s->acq_tid  = -1;
#endif
}

/* prim_pool_assign — alias *s to one of the shared pool's HW spinlocks.
 *
 * The stripe is chosen from the object's own address rather than a shared
 * counter: that spreads objects across stripes — better than funnelling
 * everything through a single lock, as the original shared_prim_lock did —
 * without needing any additional synchronised state (and hence no extra
 * lock) to make the choice.  Must be called before *s is used from more than
 * one core. */
static void prim_pool_assign(spinlock_t *s, const void *owner)
{
    uintptr_t idx = ((uintptr_t)owner >> 4) % PRIM_POOL_SIZE;

    s->hw   = prim_pool[idx].hw;
    s->lock = 0u;
#ifdef PICOOS_LOCK_DEBUG
    s->acq_file = NULL;
    s->acq_line = 0;
    s->acq_tid  = -1;
#endif
}

void sync_spinlock_report(uint32_t *used, uint32_t *total)
{
    uint32_t n = 0;

    for (uint id = PICO_SPINLOCK_ID_CLAIM_FREE_FIRST; id <= PICO_SPINLOCK_ID_CLAIM_FREE_LAST; id++) {
        if (spin_lock_is_claimed(id)) {
            n++;
        }
    }
    *used  = n;
    *total = (uint32_t)(PICO_SPINLOCK_ID_CLAIM_FREE_LAST - PICO_SPINLOCK_ID_CLAIM_FREE_FIRST + 1);
}

void sync_init(void)
{
    /* Stripes 0 and 1 ride the free OS1/OS2 IDs; the rest draw from the
     * regular claim-free range via spinlock_init(). */
    claim_spinlock_by_id(&prim_pool[0], PICO_SPINLOCK_ID_OS1);
    claim_spinlock_by_id(&prim_pool[1], PICO_SPINLOCK_ID_OS2);
    for (uint32_t i = 2; i < PRIM_POOL_SIZE; i++) {
        spinlock_init(&prim_pool[i]);
    }

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
    prim_pool_assign(&m->spin, m);  /* shared pooled HW spinlock — see prim_pool */
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
    prim_pool_assign(&s->spin, s);   /* shared pooled HW spinlock — see prim_pool */
    s->count   = initial_count;
    s->waiters = NULL;
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
    prim_pool_assign(&e->spin, e);   /* shared pooled HW spinlock — see prim_pool */
    e->flags   = 0u;
    e->waiters = NULL;
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
    prim_pool_assign(&q->spin, q);   /* shared pooled HW spinlock — see prim_pool */
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
