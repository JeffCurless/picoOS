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

#include "sched.h"
#include "task.h"
#include "arch.h"
#include "sync.h"   /* spinlock_t, PICOOS_LOCK_TIMEOUT_MS */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* RP2040 CMSIS-style register definitions (SCB, NVIC, SysTick_Config, etc.)
 * are pulled in through arch.h, which centralises all Pico SDK / CMSIS
 * includes for the kernel.  Never include pico/stdlib.h directly. */

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define TIME_SLICE_MS  10u   /* preemption interval in milliseconds */
#define NUM_PRIORITIES  8u   /* matches the priority range 0..7     */

/* -------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */

/* Exposed as extern (declared in sched.h). */
volatile uint32_t  tick_count  = 0;

/* current_tcb is defined in task.c and declared extern in both task.h and
 * sched.h.  sched.c only reads/writes it; it does NOT redefine it here. */

/* One singly-linked READY list per priority level (0 = highest). */
static tcb_t *ready_queues[NUM_PRIORITIES];

/* -------------------------------------------------------------------------
 * Deadlock detection state (debug builds only)
 *
 * g_deadlock_victim is set by the SysTick scanner when a BLOCKED thread
 * has exceeded PICOOS_LOCK_TIMEOUT_MS.  sched_next_thread() checks it and
 * calls lock_deadlock_panic() in PendSV context (same env as stack overflow).
 * ------------------------------------------------------------------------- */
#ifdef PICOOS_LOCK_DEBUG
static volatile tcb_t *g_deadlock_victim = NULL;

void __attribute__((noreturn)) lock_deadlock_panic(
    const char *lock_type,
    const char *wait_file, int wait_line,
    uint32_t    wait_tid,  const char *wait_name,
    const char *hold_file, int hold_line, int32_t hold_tid)
{
    __enable_irq();   /* allow USB IRQ to drain the CDC TX ring-buffer */

    printf("\r\n\r\n"
           "!!! DEADLOCK DETECTED !!!\r\n"
           "  Lock     : %s\r\n"
           "  Waiting  : TID %u \"%s\"  at %s:%d  (> %u ms)\r\n"
           "  Holder   : TID %d  at %s:%d\r\n"
           "System halted. Reboot required.\r\n\r\n",
           lock_type ? lock_type : "?",
           (unsigned)wait_tid, wait_name ? wait_name : "?",
           wait_file ? wait_file : "?", wait_line,
           (unsigned)PICOOS_LOCK_TIMEOUT_MS,
           (int)hold_tid,
           hold_file ? hold_file : "(no single holder)", hold_line);

    /* Pump USB for ~0.5 s so the message reaches the host terminal. */
    for (int i = 0; i < 50; i++) {
        tud_task();
        sleep_ms(10);
    }

    for (;;) { __wfi(); }
}
#endif

/* Per-core time-slice countdown (index = core_num). */
static uint32_t current_slice_remaining[2];

/* Spinlock protecting ready_queues[] across both cores.
 * Acquired by sched_add/remove_thread, sched_block/unblock/sleep, and
 * sched_next_thread.  Lock order: sched_lock → heap_lock (in task_free_thread).
 * Never acquire sched_lock while holding a sync-primitive spinlock would
 * violate the ordering — callers of sched_block/unblock do acquire them in
 * the correct order: sync-spinlock first, sched_lock inside. */
static spinlock_t sched_lock = {0};

/* -------------------------------------------------------------------------
 * trace_enabled — defined in main.c; toggled by the shell 'trace' command.
 * The SysTick ISR checks this flag but does not yet emit output (async
 * printing from an ISR requires a ring-buffer TX path — not yet implemented).
 * The flag is available for future use by any subsystem that wants to gate
 * diagnostic output.
 * ------------------------------------------------------------------------- */
extern volatile bool trace_enabled;

/* -------------------------------------------------------------------------
 * sched_add_thread_raw / sched_remove_thread_raw
 *
 * Bare linked-list operations on ready_queues[].  Caller MUST hold
 * sched_lock before calling either of these.
 * ------------------------------------------------------------------------- */
static void sched_add_thread_raw(tcb_t *t)
{
    if (t == NULL) {
        return;
    }

    uint8_t prio = t->priority;
    if (prio >= NUM_PRIORITIES) {
        prio = NUM_PRIORITIES - 1;
    }

    t->next = NULL;

    if (ready_queues[prio] == NULL) {
        ready_queues[prio] = t;
        return;
    }

    tcb_t *cur = ready_queues[prio];
    while (cur->next != NULL) {
        cur = cur->next;
    }
    cur->next = t;
}

static void sched_remove_thread_raw(tcb_t *t)
{
    if (t == NULL) {
        return;
    }

    uint8_t prio = t->priority;
    if (prio >= NUM_PRIORITIES) {
        prio = NUM_PRIORITIES - 1;
    }

    tcb_t *prev = NULL;
    tcb_t *cur  = ready_queues[prio];

    while (cur != NULL) {
        if (cur == t) {
            if (prev == NULL) {
                ready_queues[prio] = cur->next;
            } else {
                prev->next = cur->next;
            }
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

/* -------------------------------------------------------------------------
 * sched_add_thread / sched_remove_thread — public, lock-acquiring wrappers
 * ------------------------------------------------------------------------- */
void sched_add_thread(tcb_t *t)
{
    uint32_t save = spinlock_irq_acquire(&sched_lock);
    sched_add_thread_raw(t);
    spinlock_irq_release(&sched_lock, save);
}

void sched_remove_thread(tcb_t *t)
{
    uint32_t save = spinlock_irq_acquire(&sched_lock);
    sched_remove_thread_raw(t);
    spinlock_irq_release(&sched_lock, save);
}

/* -------------------------------------------------------------------------
 * sched_block
 * ------------------------------------------------------------------------- */
void sched_block(tcb_t *t)
{
    if (t == NULL) {
        return;
    }
    uint32_t save = spinlock_irq_acquire(&sched_lock);
    t->state = THREAD_BLOCKED;
    sched_remove_thread_raw(t);
    spinlock_irq_release(&sched_lock, save);
}

/* -------------------------------------------------------------------------
 * sched_unblock
 * ------------------------------------------------------------------------- */
void sched_unblock(tcb_t *t)
{
    if (t == NULL) {
        return;
    }
    uint32_t save = spinlock_irq_acquire(&sched_lock);

    if (t->state == THREAD_ZOMBIE) {
        /* Thread was killed while blocked on a sync primitive.  The primitive
         * has now dequeued its waiter-list reference, so it is safe to free
         * the TCB.  Release the scheduler lock first to avoid holding two
         * spinlocks across task_free_thread's heap operations. */
        spinlock_irq_release(&sched_lock, save);
        task_free_thread(t);
        return;
    }

    t->state = THREAD_READY;
    sched_add_thread_raw(t);
    spinlock_irq_release(&sched_lock, save);
}

/* -------------------------------------------------------------------------
 * sched_sleep
 * ------------------------------------------------------------------------- */
void sched_sleep(uint32_t ms)
{
    if (CURRENT_TCB == NULL) {
        return;
    }

    CURRENT_TCB->wake_time_us = time_us_64() + (uint64_t)ms * 1000u;

    /* Atomically mark SLEEPING and remove from the ready queue under the
     * scheduler lock so that sched_next_thread() on either core cannot
     * observe a SLEEPING thread still in the queue mid-transition. */
    uint32_t save = spinlock_irq_acquire(&sched_lock);
    CURRENT_TCB->state = THREAD_SLEEPING;
    sched_remove_thread_raw((tcb_t *)CURRENT_TCB);
    spinlock_irq_release(&sched_lock, save);

    sched_yield();
}

/* -------------------------------------------------------------------------
 * sched_yield
 *
 * Trigger a PendSV exception.  The exception will fire as soon as the
 * current exception priority allows it (i.e. after any higher-priority ISR
 * returns), resulting in a call to sched_next_thread() from sched_asm.S.
 * ------------------------------------------------------------------------- */
void sched_yield(void)
{
    /* Set the PENDSVSET bit in the Interrupt Control and State Register. */
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    __dsb();
    __isb();
}

/* -------------------------------------------------------------------------
 * stack_overflow_panic
 *
 * Called when the canary at the bottom of a thread's stack has been
 * overwritten, indicating a stack overflow.
 *
 * We are inside the PendSV handler with interrupts disabled (cpsid i from
 * step 1 of isr_pendsv).  Re-enable interrupts so the USB CDC ring-buffer
 * can drain, print the panic message, pump the USB stack for ~0.5 s, then
 * halt forever.  The user must reboot the device.
 * ------------------------------------------------------------------------- */
static void __attribute__((noreturn)) stack_overflow_panic(const tcb_t *t)
{
    __enable_irq();   /* allow USB IRQ to drain the TX ring-buffer */

    printf("\r\n\r\n"
           "!!! STACK OVERFLOW !!!\r\n"
           "  Thread : TID %u \"%s\"\r\n"
           "  Base   : 0x%08X\r\n"
           "  Canary : 0x%08X  (expected 0x%08X)\r\n"
           "System halted. Reboot required.\r\n\r\n",
           (unsigned)t->tid, t->name,
           (unsigned)(uintptr_t)t->stack_base,
           *(const uint32_t *)t->stack_base,
           STACK_CANARY);

    /* Pump USB for ~0.5 s so the message reaches the host terminal. */
    for (int i = 0; i < 50; i++) {
        tud_task();
        sleep_ms(10);
    }

    for (;;) { __wfi(); }
}

/* -------------------------------------------------------------------------
 * sched_next_thread
 *
 * Called from the PendSV handler (in sched_asm.S) with interrupts disabled.
 *
 * Algorithm: priority round-robin.
 *   1. Scan priority queues 0..7 for the first non-empty one.
 *   2. Within that queue, advance past the current thread (if it is at the
 *      head) to give other threads at the same priority a turn.
 *   3. Move the selected thread to RUNNING; leave the old thread in whatever
 *      state it was already placed in (the caller sets it before yielding).
 *
 * If no thread is ready at all, return current_tcb unchanged (the idle
 * thread should always be READY at priority 7 to prevent this case).
 * ------------------------------------------------------------------------- */
tcb_t *sched_next_thread(void)
{
    uint32_t core = get_core_num();
    tcb_t   *cur  = current_tcb[core];

    /* Acquire the scheduler spinlock.  IRQs are already disabled by the
     * PendSV entry sequence (cpsid i in sched_asm.S), so the inner
     * save_and_disable_interrupts() in spinlock_irq_acquire is a no-op. */
    uint32_t lock_save = spinlock_irq_acquire(&sched_lock);

#ifdef PICOOS_LOCK_DEBUG
    if (g_deadlock_victim != NULL) {
        const tcb_t *v = (const tcb_t *)g_deadlock_victim;
        lock_deadlock_panic(
            v->blk_what        ? v->blk_what        : "?",
            v->blk_file        ? v->blk_file        : "?", v->blk_line,
            v->tid, v->name,
            v->blk_holder_file,  v->blk_holder_line, v->blk_holder_tid);
    }
#endif

    /* Stack canary check for the outgoing thread. */
    if (cur != NULL && cur->stack_base != NULL) {
        if (*(const uint32_t *)cur->stack_base != STACK_CANARY) {
            stack_overflow_panic(cur);
        }
    }

    /* Mark outgoing thread READY so the rotation below can move it to tail. */
    if (cur != NULL && cur->state == THREAD_RUNNING) {
        cur->state = THREAD_READY;
    }

    /* Reap zombie: the thread set itself ZOMBIE and yielded; assembly has
     * already saved its context, so freeing the stack here is safe. */
    bool zombie_reaped = false;
    if (cur != NULL && cur->state == THREAD_ZOMBIE) {
        sched_remove_thread_raw(cur);
        task_free_thread(cur);   /* frees stack + TCB slot; may acquire heap_lock */
        zombie_reaped = true;
        cur = NULL;   /* TCB is zeroed — do not dereference */
    }

    /* Select the next thread: highest-priority eligible READY thread.
     * Affinity: THREAD_AFFINITY_ANY (-1) = any core, 0 = core 0, 1 = core 1. */
    tcb_t *selected = NULL;

    for (uint8_t prio = 0; prio < NUM_PRIORITIES && selected == NULL; prio++) {
        if (ready_queues[prio] == NULL) {
            continue;
        }

        /* Round-robin: if the outgoing thread is at the head of this queue
         * and is still READY, rotate it to the tail to give peers a turn. */
        if (!zombie_reaped &&
            ready_queues[prio] == cur &&
            cur != NULL &&
            cur->state == THREAD_READY)
        {
            tcb_t *head = ready_queues[prio];
            if (head->next != NULL) {
                ready_queues[prio] = head->next;
                tcb_t *tail = ready_queues[prio];
                while (tail->next != NULL) {
                    tail = tail->next;
                }
                tail->next = head;
                head->next = NULL;
            }
        }

        /* Pick the first READY thread whose affinity matches this core. */
        for (tcb_t *t = ready_queues[prio]; t != NULL; t = t->next) {
            if (t->state == THREAD_READY &&
                (t->affinity == THREAD_AFFINITY_ANY ||
                 t->affinity == (int8_t)core)) {
                selected = t;
                break;
            }
        }
    }

    if (selected == NULL) {
        /* No eligible thread — should never happen (per-core idle exists).
         * Return NULL here; the assembly will write NULL to current_tcb[core]
         * and spin in a WFI loop until the next SysTick wakes a thread. */
        spinlock_irq_release(&sched_lock, lock_save);
        return (tcb_t *)current_tcb[core];
    }

    selected->state = THREAD_RUNNING;
    current_slice_remaining[core] = TIME_SLICE_MS;

    spinlock_irq_release(&sched_lock, lock_save);
    return selected;
}

/* -------------------------------------------------------------------------
 * isr_systick — SysTick interrupt service routine (1 ms period)
 *
 * The name "isr_systick" is the Pico SDK's weak-symbol name for the SysTick
 * handler.  Defining it here overrides the default no-op.
 * ------------------------------------------------------------------------- */
void isr_systick(void)
{
    uint32_t core = get_core_num();

    /* Accumulate CPU time for the currently running thread on this core. */
    if (current_tcb[core] != NULL) {
        current_tcb[core]->cpu_time_us += 1000u;   /* 1 ms = 1000 us */
    }

    /* Time-slice preemption (per-core counter). */
    if (current_slice_remaining[core] > 0u) {
        current_slice_remaining[core]--;
    }
    if (current_slice_remaining[core] == 0u) {
        current_slice_remaining[core] = TIME_SLICE_MS;
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }

    /* Core 0 only: global tick counter, sleep wakeups, deadlock scanner.
     * Running these once per tick (not once per core per tick) avoids
     * double-waking sleepers and double-incrementing tick_count. */
    if (core != 0u) {
        return;
    }

    tick_count++;

    /* Wake any sleeping threads whose alarm has expired. */
    uint64_t now = time_us_64();
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = task_get_thread_slot(i);
        if (t == NULL) {
            continue;
        }
        if (t->state == THREAD_SLEEPING && t->wake_time_us <= now) {
            t->state = THREAD_READY;
            sched_add_thread(t);
        }
    }

#ifdef PICOOS_LOCK_DEBUG
    /* Deadlock scanner: find BLOCKED threads that have been waiting longer
     * than PICOOS_LOCK_TIMEOUT_MS.  Only the first victim is recorded;
     * sched_next_thread() reads it and calls lock_deadlock_panic(). */
    if (g_deadlock_victim == NULL) {
        for (int _di = 0; _di < MAX_THREADS; _di++) {
            tcb_t *_dt = task_get_thread_slot(_di);
            if (_dt == NULL) continue;
            if (_dt->state == THREAD_BLOCKED && _dt->blk_time_us != 0u &&
                now > _dt->blk_time_us + (uint64_t)PICOOS_LOCK_TIMEOUT_MS * 1000u) {
                g_deadlock_victim = _dt;
                SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
                break;
            }
        }
    }
#endif
}

/* -------------------------------------------------------------------------
 * sched_tick — public wrapper (declared in sched.h)
 *
 * Provided for callers that want to drive the scheduler tick manually (e.g.
 * from a hardware timer callback).  In the default configuration the SysTick
 * ISR above is used instead, but having this wrapper makes the teaching code
 * explicit about the tick-to-scheduler relationship.
 * ------------------------------------------------------------------------- */
void sched_tick(void)
{
    isr_systick();
}

/* -------------------------------------------------------------------------
 * sched_init
 *
 * Configure interrupt priorities:
 *   SysTick: medium priority (0x40)
 *   PendSV : lowest priority (0xFF) so it fires after every other ISR
 * ------------------------------------------------------------------------- */
void sched_init(void)
{
    /* Claim the RP2040 hardware spinlock for SMP-safe ready-queue access.
     * Must be done before Core 1 starts scheduling (i.e. before sched_start). */
    spinlock_init(&sched_lock);

    /* Note: ready_queues[] is populated by sched_add_thread() as threads
     * are created.  We do NOT reset the queues here — by the time sched_init
     * is called all initial threads have already been added.
     *
     * We DO reset the time-slice counter in case sched_init is called more
     * than once (e.g. during testing). */
    current_slice_remaining[0] = TIME_SLICE_MS;

    /* Set PendSV to the lowest possible priority so that context switches
     * never preempt real device ISRs. */
    NVIC_SetPriority(PendSV_IRQn, 0xFFu);

    /* SysTick at a medium priority — above PendSV but below critical ISRs. */
    NVIC_SetPriority(SysTick_IRQn, 0x40u);
}

/* -------------------------------------------------------------------------
 * sched_start
 *
 * Teaching approach: rather than engineering a first exception-return (which
 * is fiddly on M0+), we simply:
 *  1. Find the highest-priority READY thread.
 *  2. Set it as current_tcb.
 *  3. Set the PSP to just above its hardware exception frame (so that when
 *     the CPU later saves/restores context it uses the right stack).
 *  4. Switch to PSP (CONTROL.SPSEL = 1).
 *  5. Start the SysTick timer.
 *  6. Enable global interrupts.
 *  7. Call the thread's entry function directly — the SysTick/PendSV
 *     machinery will preempt it as soon as the first tick fires.
 *
 * This function never returns.
 * ------------------------------------------------------------------------- */
void sched_start(void)
{
    /* Pick the first Core-0-eligible READY thread. */
    tcb_t *first = NULL;
    for (uint8_t p = 0; p < NUM_PRIORITIES && first == NULL; p++) {
        for (tcb_t *t = ready_queues[p]; t != NULL; t = t->next) {
            if (t->affinity == THREAD_AFFINITY_ANY || t->affinity == THREAD_AFFINITY_C0) {
                first = t;
                break;
            }
        }
    }

    if (first == NULL) {
        printf("\r\nPANIC: sched_start — no ready thread for Core 0\r\n");
        for (;;) { __wfi(); }
    }

    first->state   = THREAD_RUNNING;
    current_tcb[0] = first;

    /* Compute the initial PSP value and read entry/arg from the exception frame
     * that task_create_thread() laid down.  These MUST be read before switching
     * to PSP, because any function call after __set_PSP(psp_init) pushes data
     * onto the new stack starting at psp_init — which sits exactly at the top of
     * the initial exception frame.  Calling printf() or anything else after the
     * switch corrupts saved_sp[8..15] (the pc field at saved_sp[14] in
     * particular) and turns the subsequent entry(arg) into a hard-fault.
     *
     * Frame layout set by task_create_thread():
     *   saved_sp[0..7]  = r4-r11   (software frame)
     *   saved_sp[8]     = r0 (arg)
     *   saved_sp[14]    = pc (entry)
     *   saved_sp[15]    = xpsr
     *   psp_init        = &saved_sp[16]  (one past xpsr — the initial PSP value)
     */
    uint32_t *hw_frame_bottom = first->saved_sp + 8;
    uint32_t  psp_init        = (uint32_t)(uintptr_t)(hw_frame_bottom + 8);

    /* Read entry and arg into register-class locals before any function call
     * and before the PSP switch.  Declared 'register' so the compiler is
     * explicitly told to keep these in CPU registers; if they were spilled to
     * the stack after __set_PSP they would be loaded from the wrong stack. */
    register void (*entry)(void *) = (void (*)(void *))((uintptr_t)first->saved_sp[14]);
    register void  *arg            = (void *)(uintptr_t)first->saved_sp[8];

    /* Diagnostic checkpoint — all printfs BEFORE the stack switch.
    printf("[sched] starting TID %u \"%s\"  entry=0x%08X  PSP=0x%08X\r\n",
           (unsigned)first->tid, first->name,
           (unsigned)(uintptr_t)entry, (unsigned)psp_init);
    */
    stdio_flush();

    /* 3. Set PSP to the top of the first thread's initial exception frame. */
    __set_PSP(psp_init);

    /* 4. Switch Thread mode to use PSP (CONTROL.SPSEL = 1).
     *    After __isb() every subsequent Thread-mode stack operation uses PSP.
     *    DO NOT call any C function between here and entry(arg): the compiler
     *    may spill locals to the new stack and overwrite the exception frame. */
    __set_CONTROL(__get_CONTROL() | 0x02u);
    __isb();

    /* 5. Start the SysTick timer (1 ms period). */
    SysTick_Config(clock_get_hz(clk_sys) / 1000u);

    /* 6. Enable global interrupts — SysTick and PendSV may now fire. */
    __enable_irq();

    /* 7. Call the thread's entry function directly.  The SysTick/PendSV
     *    machinery will preempt it on the first tick if a higher-priority
     *    thread becomes ready.  entry and arg are in CPU registers here
     *    (not on any stack), so the PSP switch above cannot corrupt them. */
    entry(arg);

    /* Unreachable in normal operation. */
    /* If entry returns, loop forever (the idle thread never returns). */
    for (;;) { __wfi(); }
}

/* -------------------------------------------------------------------------
 * sched_init_core1
 *
 * Configure Core 1's own NVIC interrupt priorities (each Cortex-M0+ core has
 * an independent NVIC).  Call from Core 1 before sched_start_core1().
 * ------------------------------------------------------------------------- */
void sched_init_core1(void)
{
    NVIC_SetPriority(PendSV_IRQn,  0xFFu);
    NVIC_SetPriority(SysTick_IRQn, 0x40u);
    current_slice_remaining[1] = TIME_SLICE_MS;
}

/* -------------------------------------------------------------------------
 * sched_start_core1
 *
 * Mirrors sched_start() but selects the first Core-1-eligible thread
 * (affinity == THREAD_AFFINITY_ANY or affinity == THREAD_AFFINITY_C1).
 * Sets current_tcb[1], switches Core 1 to PSP, starts Core 1's SysTick,
 * enables IRQs, and calls the thread's entry function directly.
 * This function never returns.
 * ------------------------------------------------------------------------- */
void sched_start_core1(void)
{
    /* Find the first Core-1-eligible READY thread. */
    tcb_t *first = NULL;
    for (uint8_t p = 0; p < NUM_PRIORITIES && first == NULL; p++) {
        for (tcb_t *t = ready_queues[p]; t != NULL; t = t->next) {
            if (t->state == THREAD_READY &&
                (t->affinity == THREAD_AFFINITY_ANY ||
                 t->affinity == THREAD_AFFINITY_C1)) {
                first = t;
                break;
            }
        }
    }

    if (first == NULL) {
        printf("\r\nPANIC: sched_start_core1 — no ready thread for Core 1\r\n");
        for (;;) { __wfi(); }
    }

    first->state   = THREAD_RUNNING;
    current_tcb[1] = first;

    /* Read entry/arg from the initial exception frame before PSP switch
     * (same reasoning as sched_start — see comment there). */
    uint32_t *hw_frame_bottom = first->saved_sp + 8;
    uint32_t  psp_init        = (uint32_t)(uintptr_t)(hw_frame_bottom + 8);

    register void (*entry)(void *) = (void (*)(void *))((uintptr_t)first->saved_sp[14]);
    register void  *arg            = (void *)(uintptr_t)first->saved_sp[8];

    __set_PSP(psp_init);
    __set_CONTROL(__get_CONTROL() | 0x02u);
    __isb();

    /* Start Core 1's own SysTick (1 ms period). */
    SysTick_Config(clock_get_hz(clk_sys) / 1000u);

    __enable_irq();

    entry(arg);

    for (;;) { __wfi(); }
}
