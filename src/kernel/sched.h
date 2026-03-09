#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <stdint.h>
#include "task.h"

/* -------------------------------------------------------------------------
 * Globals — defined in sched.c, declared here for other modules
 * ------------------------------------------------------------------------- */

/* Millisecond tick counter.  Incremented by isr_systick every 1 ms. */
extern volatile uint32_t tick_count;

/* The TCB currently executing on this core (same variable as in task.h;
 * both headers guard-include each other safely via the include guards). */
extern tcb_t * volatile current_tcb;

/* -------------------------------------------------------------------------
 * Scheduler API
 * ------------------------------------------------------------------------- */

/*
 * sched_init  — configure SysTick and PendSV interrupt priorities.
 *               Call once after all initial threads have been created.
 */
void sched_init(void);

/*
 * sched_start — pick the first ready thread, set it as current_tcb,
 *               configure the PSP, switch to PSP mode, enable SysTick,
 *               and call the thread's entry function directly.
 *               This function never returns.
 */
void sched_start(void);

/*
 * sched_tick  — must be called from the SysTick ISR every 1 ms.
 *               Wakes sleeping threads and triggers preemption.
 */
void sched_tick(void);

/*
 * sched_yield — voluntarily surrender the CPU.  Triggers a PendSV
 *               exception so that sched_next_thread() picks the next
 *               ready thread before returning.
 */
void sched_yield(void);

/*
 * sched_block — move thread t to BLOCKED state.  The thread will not be
 *               scheduled until sched_unblock() is called on it.
 */
void sched_block(tcb_t *t);

/*
 * sched_unblock — move thread t from BLOCKED back to READY and add it
 *                 to the appropriate priority queue.
 */
void sched_unblock(tcb_t *t);

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

#endif /* KERNEL_SCHED_H */
