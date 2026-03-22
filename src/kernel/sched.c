#include "sched.h"
#include "task.h"
#include "arch.h"

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

/* Countdown to the next forced context switch. */
static uint32_t current_slice_remaining = TIME_SLICE_MS;

/* -------------------------------------------------------------------------
 * trace_enabled — defined in main.c; toggled by the shell 'trace' command.
 * The SysTick ISR checks this flag but does not yet emit output (async
 * printing from an ISR requires a ring-buffer TX path — not yet implemented).
 * The flag is available for future use by any subsystem that wants to gate
 * diagnostic output.
 * ------------------------------------------------------------------------- */
extern volatile bool trace_enabled;

/* -------------------------------------------------------------------------
 * sched_add_thread
 *
 * Append t to the tail of the ready queue for its priority.  Using the tail
 * gives natural round-robin ordering within a priority level.
 * ------------------------------------------------------------------------- */
void sched_add_thread(tcb_t *t)
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

    /* Walk to the tail. */
    tcb_t *cur = ready_queues[prio];
    while (cur->next != NULL) {
        cur = cur->next;
    }
    cur->next = t;
}

/* -------------------------------------------------------------------------
 * sched_remove_thread
 *
 * Remove t from whatever priority queue it is currently in.
 * Does not change t->state.  Safe to call from PendSV context.
 * ------------------------------------------------------------------------- */
void sched_remove_thread(tcb_t *t)
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
 * sched_block
 * ------------------------------------------------------------------------- */
void sched_block(tcb_t *t)
{
    if (t == NULL) {
        return;
    }
    t->state = THREAD_BLOCKED;
    sched_remove_thread(t);
}

/* -------------------------------------------------------------------------
 * sched_unblock
 * ------------------------------------------------------------------------- */
void sched_unblock(tcb_t *t)
{
    if (t == NULL) {
        return;
    }
    t->state = THREAD_READY;
    sched_add_thread(t);
}

/* -------------------------------------------------------------------------
 * sched_sleep
 * ------------------------------------------------------------------------- */
void sched_sleep(uint32_t ms)
{
    if (current_tcb == NULL) {
        return;
    }

    current_tcb->wake_time_us = time_us_64() + (uint64_t)ms * 1000u;
    current_tcb->state = THREAD_SLEEPING;

    /* Remove from the ready queue so it doesn't get scheduled while asleep. */
    sched_remove_thread((tcb_t *)current_tcb);

    /* Yield so the scheduler picks the next runnable thread. */
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
    /* ---- Stack canary check for the outgoing thread ---- */
    if (current_tcb != NULL && current_tcb->stack_base != NULL) {
        if (*(const uint32_t *)current_tcb->stack_base != STACK_CANARY) {
            stack_overflow_panic(current_tcb);   /* no return */
        }
    }

    tcb_t *selected = NULL;

    /* A preempted thread arrives here with state THREAD_RUNNING (the assembly
     * does not change it).  Mark it THREAD_READY so the rotation logic below
     * can move it to the tail of its priority queue and let peers run. */
    if (current_tcb != NULL && current_tcb->state == THREAD_RUNNING) {
        current_tcb->state = THREAD_READY;
    }

    /* Reap zombie: handles the natural-exit (thread_exit) path.
     * The outgoing thread set itself ZOMBIE and yielded; the assembly has
     * already saved its context, so freeing the stack here is safe.
     * Interrupts are disabled by the PendSV entry in sched_asm.S. */
    bool zombie_reaped = false;
    if (current_tcb != NULL && current_tcb->state == THREAD_ZOMBIE) {
        sched_remove_thread((tcb_t *)current_tcb);
        task_free_thread((tcb_t *)current_tcb);   /* frees stack + TCB slot */
        zombie_reaped = true;
        /* Do not dereference current_tcb after this point — TCB is zeroed. */
    }

    for (uint8_t prio = 0; prio < NUM_PRIORITIES; prio++) {
        if (ready_queues[prio] == NULL) {
            continue;
        }

        /* There is at least one READY thread at this priority.
         *
         * Round-robin: if the current thread is at the head of this queue,
         * rotate it to the tail so the next thread in line gets to run.
         * This only makes sense if the current thread is still READY (it
         * might have been moved to SLEEPING or BLOCKED before sched_yield
         * was called).
         */
        if (!zombie_reaped &&
            ready_queues[prio] == (tcb_t *)current_tcb &&
            current_tcb != NULL &&
            current_tcb->state == THREAD_READY)
        {
            /* Rotate: pop the head and push it to the tail. */
            tcb_t *head = ready_queues[prio];
            if (head->next != NULL) {
                /* Move head to tail. */
                ready_queues[prio] = head->next;
                tcb_t *tail = ready_queues[prio];
                while (tail->next != NULL) {
                    tail = tail->next;
                }
                tail->next = head;
                head->next = NULL;
            }
            /* (If head->next is NULL it is the only thread; keep it.) */
        }

        selected = ready_queues[prio];
        break;
    }

    if (selected == NULL) {
        /* No ready thread.  If the outgoing thread was just reaped
         * (zombie_reaped == true), current_tcb has been freed and returning
         * it would dereference a zeroed TCB — a crash.  This path should
         * never be reached because the idle thread is always READY at
         * priority 7.  If it does occur, the system is in an unrecoverable
         * state regardless. */
        return (tcb_t *)current_tcb;
    }

    /* Transition the selected thread to RUNNING. */
    selected->state = THREAD_RUNNING;

    /* Reset the time slice for the incoming thread. */
    current_slice_remaining = TIME_SLICE_MS;

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
    tick_count++;

    /* Accumulate CPU time for the currently running thread. */
    if (current_tcb != NULL) {
        current_tcb->cpu_time_us += 1000u;   /* 1 ms = 1000 us */
    }

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

            if (trace_enabled) {
                /* A fully async print here would need a ring-buffer;
                 * for teaching we skip it in the ISR to avoid re-entrancy. */
            }
        }
    }

    /* Time-slice preemption. */
    if (current_slice_remaining > 0) {
        current_slice_remaining--;
    }
    if (current_slice_remaining == 0) {
        current_slice_remaining = TIME_SLICE_MS;
        /* Pend a context switch. */
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
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
    /* Note: ready_queues[] is populated by sched_add_thread() as threads
     * are created.  We do NOT reset the queues here — by the time sched_init
     * is called all initial threads have already been added.
     *
     * We DO reset the time-slice counter in case sched_init is called more
     * than once (e.g. during testing). */
    current_slice_remaining = TIME_SLICE_MS;

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
    /* Pick the first ready thread. */
    tcb_t *first = NULL;
    for (uint8_t p = 0; p < NUM_PRIORITIES; p++) {
        if (ready_queues[p] != NULL) {
            first = ready_queues[p];
            break;
        }
    }

    if (first == NULL) {
        /* Nothing to run — should never happen if the idle thread exists. */
        for (;;) { __wfi(); }
    }

    first->state = THREAD_RUNNING;
    current_tcb  = first;

    /* Point the PSP at the top of the hardware exception frame within
     * the thread's initial stack (just above the xpsr word).
     * saved_sp points to the r4 word (the bottom of the 16-word frame).
     * The hardware frame starts 8 words above saved_sp. */
    uint32_t *hw_frame_bottom = first->saved_sp + 8;  /* r0 word */
    uint32_t  psp_init        = (uint32_t)(uintptr_t)(hw_frame_bottom + 8); /* past xpsr */

    /* Switch to PSP and set its value.
     * CONTROL register bit 1 (SPSEL) selects PSP in Thread mode. */
    __set_PSP(psp_init);
    __set_CONTROL(__get_CONTROL() | 0x02u);
    __isb();

    /* Configure SysTick for a 1 ms period.
     * Use clock_get_hz() so this is correct regardless of board or clock speed. */
    SysTick_Config(clock_get_hz(clk_sys) / 1000u);   /* 1 ms tick, any clock speed */

    /* Enable global interrupts. */
    __enable_irq();

    /* Retrieve entry and arg from the initialised stack frame.
     * The hardware frame we laid down is:
     *   saved_sp[0..7]  = r4-r11 (software frame)
     *   saved_sp[8]     = r0  (= arg)
     *   saved_sp[14]    = pc  (= entry)
     */
    void (*entry)(void *) = (void (*)(void *))((uintptr_t)first->saved_sp[14]);
    void  *arg            = (void *)(uintptr_t)first->saved_sp[8];

    /* Call the first thread's entry directly. */
    entry(arg);

    /* If entry returns, loop forever (the idle thread never returns). */
    for (;;) { __wfi(); }
}
