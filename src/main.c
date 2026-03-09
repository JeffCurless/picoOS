#include <stdio.h>
#include <stdbool.h>

#include "kernel/arch.h"
#include "kernel/task.h"
#include "kernel/sched.h"
#include "kernel/mem.h"
#include "kernel/dev.h"
#include "kernel/vfs.h"
#include "kernel/fs.h"
#include "shell/shell.h"
#include "apps/demo.h"

/* -------------------------------------------------------------------------
 * trace_enabled
 *
 * When true the scheduler (and any other subsystem that checks this flag)
 * may emit extra diagnostic output.  Toggled by the shell 'trace' command.
 * Declared volatile because the SysTick ISR reads it.
 * ------------------------------------------------------------------------- */
volatile bool trace_enabled = false;

/* Set to true by core1_entry() after multicore_lockout_victim_init() completes.
 * Core 0 spins on this flag before calling fs_init() so that any flash
 * erase/program operation (which calls multicore_lockout_start_blocking()) is
 * guaranteed to find Core 1 ready to respond. */
static volatile bool core1_lockout_ready = false;

/* -------------------------------------------------------------------------
 * Idle thread
 *
 * Always READY at the lowest priority (7).  When no other thread can run,
 * the scheduler picks this one.  __wfi() halts the core until the next
 * interrupt fires, which saves power and makes idle periods visible in
 * energy profiles — a useful teaching point.
 * ------------------------------------------------------------------------- */
static void idle_thread(void *arg)
{
    (void)arg;
    for (;;) {
        __wfi();
    }
}

/* -------------------------------------------------------------------------
 * Shell thread entry
 *
 * A thin wrapper so we can pass shell_init / shell_run as a void(*)(void*)
 * compatible with task_create_thread.
 * ------------------------------------------------------------------------- */
static void shell_thread_entry(void *arg)
{
    (void)arg;
    shell_init();
    shell_run();   /* never returns */
}

/* -------------------------------------------------------------------------
 * Core 1 entry
 *
 * Phase 3: Core 1 runs a simple scheduler loop.  In this first version it
 * checks for READY threads with affinity == 2 (core-1 pinned) or affinity
 * == 0 (any core) and runs them.  A full SMP implementation would share the
 * ready queues with proper locking; for the teaching OS we keep it simple:
 * Core 1 just runs its own idle loop unless specific work is pinned there.
 *
 * Students can extend this to a symmetric ready-queue design in Phase 3+.
 * ------------------------------------------------------------------------- */
static void core1_entry(void)
{
    /* Register Core 1 as a lockout victim so that Core 0 can call
     * multicore_lockout_start_blocking() before flash erase/program operations.
     * Without this call, any fs_write() / fs_close() / fs_delete() on Core 0
     * will block forever waiting for Core 1 to acknowledge the lockout. */
    multicore_lockout_victim_init();
    core1_lockout_ready = true;   /* signal Core 0 that lockout is armed */

    /* Core 1 sits in a low-power wait loop.  The multicore FIFO can be used
     * in a future phase to dispatch work items from Core 0. */
    for (;;) {
        __wfi();
    }
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    /* ------------------------------------------------------------------
     * 1. Initialise stdio over USB CDC.
     *
     * stdio_usb_init() starts the TinyUSB stack.  We poll with a short
     * timeout to let the host enumerate the device.  A real deployment
     * might skip the wait, but for an interactive teaching OS we want
     * the shell to be available immediately after the Pico enumerates.
     * ------------------------------------------------------------------ */
    stdio_usb_init();

    /* Wait up to 3 seconds for a USB host connection. */
    for (int i = 0; i < 30; i++) {
        if (stdio_usb_connected()) {
            break;
        }
        sleep_ms(100);
    }

    /* ------------------------------------------------------------------
     * 2. Boot banner
     * ------------------------------------------------------------------ */
    printf("\r\n\r\n=================================\r\n");
    printf("       PicoTeachOS\r\n");
    printf("  RP2040 dual-core educational OS\r\n");
    printf("  Build: %s %s\r\n", __DATE__, __TIME__);
    printf("=================================\r\n\r\n");

    /* ------------------------------------------------------------------
     * 3. Launch Core 1 early so it can register as a multicore lockout
     *    victim before fs_init() runs.
     *
     *    fs_init() may call fs_format() on first boot, which erases flash
     *    sectors using multicore_lockout_start_blocking().  That function
     *    blocks until Core 1 acknowledges the lockout, so Core 1 must have
     *    called multicore_lockout_victim_init() first.  We spin on the
     *    core1_lockout_ready flag which Core 1 sets after registering.
     * ------------------------------------------------------------------ */
    multicore_launch_core1(core1_entry);
    while (!core1_lockout_ready) {
        tight_loop_contents();   /* busy-wait; Core 1 sets flag in ~us */
    }

    /* ------------------------------------------------------------------
     * 4. Kernel subsystem initialisation (order matters)
     * ------------------------------------------------------------------ */
    mem_init();    /* heap must be ready before any kmalloc        */
    task_init();   /* TCB/PCB pools, ID counters                   */
    dev_init();    /* register device descriptors                  */
    vfs_init();    /* mount /dev entries, open-fd table            */
    fs_init();     /* mount flash FS or format if first boot       */

    /* Initialise the producer-consumer IPC objects before any demo
     * thread has a chance to run and find them uninitialised. */
    demo_ipc_init();

    /* ------------------------------------------------------------------
     * 5. Create the kernel process (PID 1)
     * ------------------------------------------------------------------ */
    pcb_t *kernel_proc = task_create_process("kernel", 1u);

    /* ------------------------------------------------------------------
     * 6. Idle thread — priority 7 (lowest), tiny stack
     *
     * Must exist so the scheduler always has at least one READY thread.
     * ------------------------------------------------------------------ */
    task_create_thread(kernel_proc, "idle",
                       idle_thread, NULL,
                       7u, IDLE_STACK_SIZE);

    /* ------------------------------------------------------------------
     * 7. Shell thread — priority 2, service stack
     * ------------------------------------------------------------------ */
    pcb_t *shell_proc = task_create_process("shell", 2u);
    task_create_thread(shell_proc, "shell",
                       shell_thread_entry, NULL,
                       2u, SERVICE_STACK_SIZE);

    /* ------------------------------------------------------------------
     * 8. Demo threads — priority 4 / 5
     * ------------------------------------------------------------------ */
    pcb_t *demo_proc = task_create_process("demos", 3u);
    task_create_thread(demo_proc, "producer",
                       demo_producer, NULL,
                       4u, DEFAULT_STACK_SIZE);
    task_create_thread(demo_proc, "consumer",
                       demo_consumer, NULL,
                       4u, DEFAULT_STACK_SIZE);
    task_create_thread(demo_proc, "sensor",
                       demo_sensor, NULL,
                       5u, DEFAULT_STACK_SIZE);

    /* ------------------------------------------------------------------
     * 9. Print thread summary before starting the scheduler
     * ------------------------------------------------------------------ */

    printf("Threads created:\r\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = task_get_thread_slot(i);
        if (t == NULL) { continue; }
        printf("  TID %u  PID %u  pri %u  %s\r\n",
               t->tid, t->pid, (unsigned)t->priority, t->name);
    }
    printf("\r\nStarting scheduler...\r\n\r\n");

    /* ------------------------------------------------------------------
     * 10. Start the scheduler — this call never returns.
     *
     * sched_init() sets interrupt priorities.
     * sched_start() picks the first ready thread, switches to PSP,
     * enables SysTick, and calls the thread entry function directly.
     * From that point on, preemption and PendSV drive context switches.
     * ------------------------------------------------------------------ */
    sched_init();
    sched_start();

    /* Unreachable — the scheduler takes over permanently. */
    return 0;
}
