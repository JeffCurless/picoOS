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

#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <stdint.h>
#include "task.h"

/* -------------------------------------------------------------------------
 * Globals — defined in sched.c, declared here for other modules
 * ------------------------------------------------------------------------- */

/* Millisecond tick counter.  Incremented by isr_systick every 1 ms. */
extern volatile uint32_t tick_count;

/* current_tcb[2] (per-core running TCB) is declared in task.h. */

/* -------------------------------------------------------------------------
 * Scheduler API
 * ------------------------------------------------------------------------- */

/*
 * sched_init  — configure SysTick and PendSV interrupt priorities on Core 0.
 *               Call once after all initial threads have been created.
 */
void sched_init(void);

/*
 * sched_init_core1  — configure Core 1's own interrupt priorities and
 *                     time-slice counter.  Call from Core 1 before
 *                     sched_start_core1().
 */
void sched_init_core1(void);

/*
 * sched_start_core1 — select the first Core-1-eligible thread, set up the
 *                     PSP, start Core 1's SysTick, and call the entry
 *                     function directly.  This function never returns.
 */
void sched_start_core1(void);

/*
 * sched_start — pick the first ready thread, set it as current_tcb,
 *               configure the PSP, switch to PSP mode, enable SysTick,
 *               and call the thread's entry function directly.
 *               This function never returns.
 */
void sched_start(void);

/*
 * sched_yield — voluntarily surrender the CPU.  Triggers a PendSV
 *               exception so that sched_next_thread() picks the next
 *               ready thread before returning.
 */
void sched_yield(void);

/*
 * sched_block — move thread t to BLOCKED state.  The thread will not be
 *               scheduled until sched_unblock() is called on it.  Returns
 *               false, and makes t a ZOMBIE instead, if a kill is pending
 *               on t and it holds no kmutex: the caller must then not put
 *               it on a waiter list, and its yield reaps it.
 */
bool sched_block(tcb_t *t);

/*
 * sched_unblock — move thread t from BLOCKED back to READY and add it
 *                 to the appropriate priority queue.  A thread that was
 *                 killed while blocked (ZOMBIE) is freed instead.  Returns
 *                 true if t was woken, false if it was freed — a primitive
 *                 handing out one wake-up should then wake the next waiter.
 */
bool sched_unblock(tcb_t *t);

/*
 * sched_kill — mark t killed, under the scheduler lock.  Use
 *              task_kill_thread() rather than this.
 *
 *   on_list   t->wait_list is set and the caller holds t->wait_lock, so a
 *             BLOCKED t can be unlinked from its waiter list right now
 *             (sync_kill() does that on KILL_FREE).
 *
 * Returns:
 *   KILL_FREE   t is off-CPU and will be out of every queue: the caller
 *               frees it (task_free_thread) once it has unlinked it.
 *   KILL_DONE   nothing for the caller to do: t is the caller, already
 *               dying, or freed later (it holds a kmutex, or is running on
 *               the other core: kill_pending).
 *   KILL_RETRY  t is BLOCKED but cannot be unlinked yet: it is still
 *               switching out on the other core, its waker is between
 *               dequeueing and waking it, or it is about to join a list.
 *               Each window is a few instructions; yield and try again.
 */
typedef enum { KILL_DONE, KILL_FREE, KILL_RETRY } kill_result_t;

kill_result_t sched_kill(tcb_t *t, bool on_list);

/*
 * sched_kill_point — called by kmutex_unlock() when the current thread has
 *              just released its last kmutex.  If a kill was deferred
 *              because it held one (kill_pending), the thread becomes a
 *              ZOMBIE here and does not return; otherwise this returns at
 *              once.  Thread context only.
 */
void sched_kill_point(void);

/*
 * sched_sleep — put the current thread to sleep for (at least) ms
 *               milliseconds, then yield the CPU.
 */
void sched_sleep(uint32_t ms);

/*
 * sched_next_thread — called from the PendSV handler to select the next
 *                     thread to run.  Implements priority round-robin.
 *                     Returns a pointer to the selected TCB.
 */
tcb_t *sched_next_thread(void);

/*
 * sched_add_thread — add a newly created READY thread to the appropriate
 *                    priority queue.  Also called by sched_unblock.
 */
void sched_add_thread(tcb_t *t);

/*
 * sched_remove_thread — remove t from whatever priority queue it currently
 *                       occupies.  Does not change t->state.  Safe to call
 *                       from PendSV context (interrupts already disabled).
 */
void sched_remove_thread(tcb_t *t);

/* -------------------------------------------------------------------------
 * Scheduler trace — a flight recorder of scheduling events (shell `trace`)
 *
 * While recording, the scheduler logs each event into a ring of the last
 * TRACE_EVENTS events, overwriting the oldest.  Nothing is printed from
 * interrupt context: a thread reads the ring later with sched_trace_get().
 * The ring is guarded by the scheduler lock, which every writer (PendSV,
 * SysTick, sched_unblock, sched_kill) already holds, so tracing claims no
 * hardware spinlock of its own.
 * ------------------------------------------------------------------------- */
#define TRACE_EVENTS    128u
#define TRACE_NAME_LEN  12u   /* thread names are copied, truncated to 11 chars */

typedef enum {
    TRACE_SWITCH  = 0,   /* core switched from tid to to_tid; state says why */
    TRACE_WAKE    = 1,   /* sleeping thread tid woke (SysTick, core 0)        */
    TRACE_UNBLOCK = 2,   /* blocked thread tid woken by a sync primitive      */
    TRACE_KILL    = 3,   /* thread tid killed                                 */
} trace_type_t;

typedef struct {
    uint32_t time_us;                 /* low 32 bits of the µs timer        */
    uint8_t  type;                    /* trace_type_t                       */
    uint8_t  core;                    /* core that recorded the event       */
    uint8_t  state;                   /* SWITCH: outgoing thread's state    */
    uint8_t  _pad;
    uint32_t tid;                     /* subject (SWITCH: outgoing) thread  */
    uint32_t to_tid;                  /* SWITCH: incoming thread            */
    char     name[TRACE_NAME_LEN];    /* names are copied: the threads may  */
    char     to_name[TRACE_NAME_LEN]; /* have exited before the dump        */
} trace_event_t;

/*
 * sched_trace_start — empty the ring and start recording.  filter, if not
 *                     NULL or "", keeps only events naming a thread whose
 *                     name starts with it ("pi" keeps the pi workers).
 */
void sched_trace_start(const char *filter);

/*
 * sched_trace_pause — stop (true) or resume (false) recording without
 *                     emptying the ring.
 */
void sched_trace_pause(bool pause);

/*
 * sched_trace_info — the sequence numbers of the oldest kept event (*first)
 *                    and one past the newest (*end); the active filter
 *                    ("" for none).  Returns true while recording.
 */
bool sched_trace_info(uint32_t *first, uint32_t *end, const char **filter);

/*
 * sched_trace_get — copy event number seq into *out.  Returns false if it
 *                   was overwritten or not yet recorded.
 */
bool sched_trace_get(uint32_t seq, trace_event_t *out);

/* -------------------------------------------------------------------------
 * Deadlock panic — defined in sched.c, callable from sync.c (spinlock
 * timeouts) and from sched_next_thread() (BLOCKED-thread scanner).
 *
 * Only compiled when PICOOS_LOCK_DEBUG is set.  Mirrors stack_overflow_panic:
 * enables IRQs, prints diagnostics, pumps USB ~500 ms, then halts.
 * ------------------------------------------------------------------------- */
#ifdef PICOOS_LOCK_DEBUG
void __attribute__((noreturn)) lock_deadlock_panic(
    const char *lock_type,
    const char *wait_file, int wait_line,
    uint32_t    wait_tid,  const char *wait_name,
    const char *hold_file, int hold_line, int32_t hold_tid);
#endif

#endif /* KERNEL_SCHED_H */
