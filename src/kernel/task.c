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


#include "task.h"
#include "sched.h"   /* sched_add_thread, sched_remove_thread */
#include "mem.h"     /* kmalloc / kfree  */
#include "sync.h"    /* spinlock_t (task_lock) */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Compile-time layout check
 *
 * The PendSV handler in sched_asm.S uses a hard-coded byte offset of 16 to
 * reach saved_sp inside tcb_t.  This assertion catches accidental struct
 * reordering at compile time.
 * ------------------------------------------------------------------------- */
/* This assert is valid only when pointers are 4 bytes (32-bit ARM target).
 * On a 64-bit host the struct layout differs, so we guard by pointer size. */
#if __SIZEOF_POINTER__ == 4
_Static_assert(offsetof(tcb_t, saved_sp)   == 16,
               "saved_sp offset mismatch: update the #16 offset in sched_asm.S");
_Static_assert(offsetof(tcb_t, exc_return) == 20,
               "exc_return offset mismatch: update the #20 offset in sched_asm.S");
#endif

/* -------------------------------------------------------------------------
 * Static pools
 *
 * TCBs and PCBs come from static arrays so the linker can account for their
 * size at build time.  Thread stacks are allocated dynamically from the
 * kernel heap (kmalloc) and freed when the thread exits (kfree).
 * ------------------------------------------------------------------------- */

static tcb_t tcb_pool[MAX_THREADS];
static pcb_t pcb_pool[MAX_PROCESSES];

/* in-use flags */
static bool tcb_used[MAX_THREADS];
static bool pcb_used[MAX_PROCESSES];

/* monotonically increasing ID counters */
static uint32_t next_tid = 1;
static uint32_t next_proc_serial = 1;   /* pcb_t.serial; 0 is never used */

/* PID ranges handed out by task_alloc_pid(), indexed by pid_range_t.
 * next is the PID to try first; it starts at base. */
typedef struct {
    uint32_t base;
    uint32_t limit;   /* inclusive */
    uint32_t next;
} pid_range_info_t;

static pid_range_info_t pid_ranges[PID_RANGE_COUNT] = {
    [PID_RANGE_SPAWN]   = {  10u,  49u,  10u },
    [PID_RANGE_AUTORUN] = {  50u,  99u,  50u },
    [PID_RANGE_RUN]     = { 100u, 199u, 100u },
    [PID_RANGE_BUTTON]  = { 200u, 299u, 200u },
};

/* -------------------------------------------------------------------------
 * task_lock — guards the pools and ID counters across both cores
 *
 * Covers tcb_used[], pcb_used[], next_tid, pid_ranges[], each PCB's
 * threads[] / thread_count, and s_kernel_proc.  Without it, two threads
 * creating threads at once (say `pi` starting its workers on core 1 while
 * the shell runs `run` on core 0) could both claim the same free slot.
 *
 * Lock order: sched_lock -> task_lock.  task_free_thread() is called from
 * sched_next_thread() / sched_unblock() with sched_lock possibly held, so
 * task_lock nests inside it.  task_lock itself is a leaf: nothing takes
 * another lock while holding it — kmalloc/kfree (heap_lock) and
 * sched_add_thread (sched_lock) run before or after the critical section.
 *
 * The lookups (task_find_*, task_get_*_slot) stay unlocked: they hand back
 * a pointer the caller uses after any lock would be released anyway, so
 * locking them would not make the result any safer to use.
 * ------------------------------------------------------------------------- */
static spinlock_t task_lock = {0};

/* Per-core current TCB: index 0 = Core 0, index 1 = Core 1. */
tcb_t * volatile current_tcb[2] = {NULL, NULL};

/* Pointer to the kernel process (PID 1), set in task_create_process when
 * pid == 1.  Exposed via task_get_kernel_proc() for use by kernel modules
 * that need to create threads in the kernel process (e.g. wifi-poll). */
static pcb_t *s_kernel_proc = NULL;

pcb_t *task_get_kernel_proc(void) { return s_kernel_proc; }

/* -------------------------------------------------------------------------
 * thread_exit — used as the initial LR so a returning thread self-terminates
 * ------------------------------------------------------------------------- */
static void thread_exit(void);   /* forward decl for stack initialisation */

/* -------------------------------------------------------------------------
 * task_init
 * ------------------------------------------------------------------------- */
void task_init(void)
{
    memset(tcb_pool, 0, sizeof(tcb_pool));
    memset(pcb_pool, 0, sizeof(pcb_pool));
    memset(tcb_used, 0, sizeof(tcb_used));
    memset(pcb_used, 0, sizeof(pcb_used));

    next_tid = 1;
    next_proc_serial = 1;
    for (int i = 0; i < PID_RANGE_COUNT; i++) {
        pid_ranges[i].next = pid_ranges[i].base;
    }
    current_tcb[0] = current_tcb[1] = NULL;

    spinlock_init(&task_lock);
}

/* -------------------------------------------------------------------------
 * Unlocked helpers — caller holds task_lock
 * ------------------------------------------------------------------------- */
static pcb_t *find_process_locked(uint32_t pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb_used[i] && pcb_pool[i].pid == pid) {
            return &pcb_pool[i];
        }
    }
    return NULL;
}

static void free_process_locked(pcb_t *p)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&pcb_pool[i] == p) {
            pcb_used[i] = false;
            memset(p, 0, sizeof(pcb_t));
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * task_create_thread
 *
 * Allocates a TCB and stack slot, initialises the ARM Cortex-M0+ exception
 * return frame so that the PendSV handler can context-switch into this
 * thread on its first run.
 *
 * Stack layout immediately after initialisation (addresses increase upward,
 * stack grows downward on ARM):
 *
 *   high address (stack top = stack_base + stack_size)
 *   +-----------+
 *   | xpsr      |  0x01000000  (Thumb bit set — mandatory on M0+)
 *   | pc        |  entry point
 *   | lr        |  &thread_exit
 *   | r12       |  0
 *   | r3        |  0
 *   | r2        |  0
 *   | r1        |  0
 *   | r0        |  arg          <-- hardware auto-saved frame (8 words)
 *   | r11       |  0
 *   | r10       |  0
 *   | r9        |  0
 *   | r8        |  0
 *   | r7        |  0
 *   | r6        |  0
 *   | r5        |  0
 *   | r4        |  0            <-- saved_sp points here (software frame)
 *   +-----------+
 *   | STACK_FILL|  every unused word, so task_stack_used() can find the
 *   |   ...     |  high-water mark later
 *   +-----------+
 *   low address (stack_base + STACK_CANARY written here)
 *
 * The PendSV handler loads saved_sp, pops r4-r11, updates PSP, then returns
 * with the thread's saved EXC_RETURN value which causes the hardware to pop r0-r3, r12,
 * lr, pc, xpsr from the PSP stack.
 *
 * The stack is allocated and painted before task_lock is taken, so the
 * lock is held only for the slot claim and never across kmalloc.
 * ------------------------------------------------------------------------- */
tcb_t *task_create_thread(pcb_t      *proc,
                           const char *name,
                           void      (*entry)(void *),
                           void       *arg,
                           uint8_t     priority,
                           uint32_t    stack_size)
{
    /* The initial frame alone is 64 bytes; anything this small cannot run
     * a C function.  Round up to 8 so the stack top keeps the 8-byte
     * alignment the AAPCS requires (kmalloc returns 8-aligned blocks). */
    if (stack_size < MIN_STACK_SIZE) {
        return NULL;
    }
    stack_size = (stack_size + 7u) & ~7u;

    /* Allocate the stack from the kernel heap and paint it. */
    uint8_t *stack = (uint8_t *)kmalloc(stack_size);
    if (stack == NULL) {
        return NULL;           /* heap exhausted */
    }
    uint32_t *words = (uint32_t *)stack;
    for (uint32_t i = 0; i < stack_size / sizeof(uint32_t); i++) {
        words[i] = STACK_FILL;
    }
    /* Canary at the very bottom of the stack (lowest address). */
    words[0] = STACK_CANARY;

    uint32_t save = spinlock_irq_acquire(&task_lock);

    /* A process being killed (or already freed: memset clears alive) gets
     * no new threads — it would outlive the kill. */
    if (proc != NULL && (!proc->alive || proc->thread_count >= MAX_THREADS)) {
        spinlock_irq_release(&task_lock, save);
        kfree(stack);
        return NULL;
    }

    /* Find a free TCB slot. */
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!tcb_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irq_release(&task_lock, save);
        kfree(stack);
        return NULL;   /* pool exhausted */
    }

    tcb_t *t = &tcb_pool[slot];
    tcb_used[slot] = true;

    /* Basic fields. */
    t->tid        = next_tid++;
    t->pid        = proc ? proc->pid : 0;
    t->stack_base = stack;
    t->stack_size = stack_size;
    t->priority   = priority;
    t->affinity   = THREAD_AFFINITY_ANY; /* run on any core */
    t->state      = THREAD_NEW;          /* READY once the frame is built */
    t->kill_pending = false;
    t->mutex_depth  = 0;
    t->wake_time_us = 0;
    t->cpu_time_us  = 0;
    t->next       = NULL;
    t->wait_next  = NULL;

    strncpy(t->name, name ? name : "thread", sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';

    /* Link thread into its owning process. */
    if (proc != NULL) {
        proc->threads[proc->thread_count++] = t;
    }

    spinlock_irq_release(&task_lock, save);

    /* Build the initial stack frame.  The slot is ours, and the thread is
     * not on a ready queue yet, so no lock is needed.
     *
     * We work from the top of the stack downward:
     *   stack_top = stack_base + stack_size
     *
     * First, lay down the 8-word hardware exception frame (auto-saved by
     * the CPU when it takes the first exception return):
     *   [r0, r1, r2, r3, r12, lr, pc, xpsr]
     *
     * Then, immediately below, lay down 8 words for the software-saved
     * callee registers [r4, r5, r6, r7, r8, r9, r10, r11].
     *
     * saved_sp points to the bottom of the software frame (the r4 word).
     */
    uint32_t *stack_top = (uint32_t *)(t->stack_base + stack_size);

    /* Index from stack_top going down (stack_top[-1] is the first word
     * pushed, i.e. the highest address in the frame). */

    /* Hardware auto-saved frame (8 words, highest addresses first): */
    stack_top[-1] = 0x01000000u;                    /* xpsr: Thumb bit set */
    stack_top[-2] = (uint32_t)(uintptr_t)entry;     /* pc: entry point     */
    stack_top[-3] = (uint32_t)(uintptr_t)thread_exit; /* lr: return addr   */
    stack_top[-4] = 0;                              /* r12                 */
    stack_top[-5] = 0;                              /* r3                  */
    stack_top[-6] = 0;                              /* r2                  */
    stack_top[-7] = 0;                              /* r1                  */
    stack_top[-8] = (uint32_t)(uintptr_t)arg;       /* r0: first argument  */

    /* Software-saved frame (8 words, directly below hardware frame): */
    stack_top[-9]  = 0;  /* r11 */
    stack_top[-10] = 0;  /* r10 */
    stack_top[-11] = 0;  /* r9  */
    stack_top[-12] = 0;  /* r8  */
    stack_top[-13] = 0;  /* r7  */
    stack_top[-14] = 0;  /* r6  */
    stack_top[-15] = 0;  /* r5  */
    stack_top[-16] = 0;  /* r4  */

    /* saved_sp points to the r4 word — the lowest address in the combined
     * frame.  That matches what the PendSV handler expects to find. */
    t->saved_sp   = &stack_top[-16];

    /* On Cortex-M33 (RP2350) the FPU may be active; the EXC_RETURN value
     * in LR when PendSV fires tells the CPU whether the saved exception frame
     * is basic (8 words, 0xFFFFFFFD) or extended with FP (26 words, 0xFFFFFFED).
     * New threads always start with a basic 8-word frame, so initialise to the
     * basic-frame EXC_RETURN.  The PendSV handler updates this on every
     * suspension so future restores use the correct frame size. */
    t->exc_return = 0xFFFFFFFDu;

    /* Notify the scheduler so it can add the thread to a ready queue. */
    t->state = THREAD_READY;
    sched_add_thread(t);

    return t;
}

/* -------------------------------------------------------------------------
 * thread_exit — called when a thread's entry function returns
 * ------------------------------------------------------------------------- */
static void thread_exit(void)
{
    /* Mark the current thread as a zombie.  sched_next_thread() detects the
     * ZOMBIE state on the next PendSV, calls task_free_thread() to reclaim
     * the stack and TCB slot, and auto-frees the PCB if no threads remain. */
    if (CURRENT_TCB != NULL) {
        CURRENT_TCB->state = THREAD_ZOMBIE;
    }
    /* Yield once; the scheduler reaps this thread on the first PendSV.
     * The loop is a safety net — in normal operation we never return here. */
    for (;;) {
        sched_yield();
    }
}

/* -------------------------------------------------------------------------
 * task_create_process
 * ------------------------------------------------------------------------- */
pcb_t *task_create_process(const char *name, uint32_t pid)
{
    /* PID 0 means "none" (parent_pid) and is what task_alloc_pid() returns
     * when a range is full. */
    if (pid == 0u) {
        return NULL;
    }

    uint32_t save = spinlock_irq_acquire(&task_lock);

    /* A duplicate would make task_find_process() ambiguous.  Checked under
     * the lock so two creators cannot both pass it. */
    if (find_process_locked(pid) != NULL) {
        spinlock_irq_release(&task_lock, save);
        return NULL;
    }

    /* Find a free PCB slot. */
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcb_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irq_release(&task_lock, save);
        return NULL;   /* pool exhausted */
    }

    pcb_t *p = &pcb_pool[slot];
    memset(p, 0, sizeof(pcb_t));   /* threads[] all NULL, counts 0 */

    p->pid          = pid;
    p->parent_pid   = 0;
    p->thread_count = 0;
    p->heap_base    = NULL;
    p->heap_size    = 0;
    p->exit_code    = 0;
    p->alive        = true;
    p->serial       = next_proc_serial++;
    if (next_proc_serial == 0u) {
        next_proc_serial = 1u;   /* 0 means "no process" */
    }

    strncpy(p->name, name ? name : "process", sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';

    strncpy(p->cwd, "/", sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = '\0';

    /* Initialise file descriptor table: -1 means unused. */
    for (int i = 0; i < MAX_FDS; i++) {
        p->fd_table[i] = -1;
    }

    if (pid == PID_KERNEL) {
        s_kernel_proc = p;
    }

    /* Publish the slot last, once the PCB is filled in. */
    pcb_used[slot] = true;

    spinlock_irq_release(&task_lock, save);
    return p;
}

/* -------------------------------------------------------------------------
 * task_alloc_pid
 *
 * Under task_lock, so two launchers drawing from the same range (two
 * threads calling SYS_SPAWN at once) never get the same PID.
 * ------------------------------------------------------------------------- */
uint32_t task_alloc_pid(pid_range_t range)
{
    if ((unsigned)range >= PID_RANGE_COUNT) {
        return 0u;
    }

    uint32_t save = spinlock_irq_acquire(&task_lock);

    pid_range_info_t *r = &pid_ranges[range];
    uint32_t span = r->limit - r->base + 1u;
    uint32_t pid  = r->next;
    uint32_t result = 0u;   /* 0 = every PID in the range is live */

    for (uint32_t i = 0; i < span; i++) {
        uint32_t candidate = pid;
        pid = (pid == r->limit) ? r->base : pid + 1u;
        if (find_process_locked(candidate) == NULL) {
            r->next = pid;
            result  = candidate;
            break;
        }
    }

    spinlock_irq_release(&task_lock, save);
    return result;
}

/* -------------------------------------------------------------------------
 * Lookup helpers (unlocked — see task_lock)
 * ------------------------------------------------------------------------- */

tcb_t *task_find_thread(uint32_t tid)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (tcb_used[i] && tcb_pool[i].tid == tid) {
            return &tcb_pool[i];
        }
    }
    return NULL;
}

pcb_t *task_find_process(uint32_t pid)
{
    return find_process_locked(pid);
}

/* Locked, unlike the lookups above: the answer is a value, not a pointer,
 * so it is exact at the moment it is taken. */
uint32_t task_process_serial(uint32_t pid)
{
    uint32_t save = spinlock_irq_acquire(&task_lock);
    pcb_t   *p    = find_process_locked(pid);
    uint32_t s    = (p != NULL) ? p->serial : 0u;
    spinlock_irq_release(&task_lock, save);
    return s;
}

/* -------------------------------------------------------------------------
 * Slot iterators (used by the shell 'ps' and 'threads' commands)
 * ------------------------------------------------------------------------- */

tcb_t *task_get_thread_slot(int idx)
{
    if (idx < 0 || idx >= MAX_THREADS) {
        return NULL;
    }
    if (!tcb_used[idx]) {
        return NULL;
    }
    return &tcb_pool[idx];
}

pcb_t *task_get_process_slot(int idx)
{
    if (idx < 0 || idx >= MAX_PROCESSES) {
        return NULL;
    }
    if (!pcb_used[idx]) {
        return NULL;
    }
    return &pcb_pool[idx];
}

/* -------------------------------------------------------------------------
 * task_stack_used
 *
 * The stack grows down from stack_base + stack_size, so the lowest words
 * are the last to be touched.  Scan up from just above the canary: every
 * word still holding STACK_FILL was never written.  A thread can write the
 * fill value itself by chance, which would make this under-report by a
 * word or so — good enough for sizing stacks.
 * ------------------------------------------------------------------------- */
uint32_t task_stack_used(const tcb_t *t)
{
    if (t == NULL || t->stack_base == NULL) {
        return 0u;
    }
    const uint32_t *words = (const uint32_t *)t->stack_base;
    uint32_t n = t->stack_size / sizeof(uint32_t);
    uint32_t i = 1u;   /* words[0] is the canary */
    while (i < n && words[i] == STACK_FILL) {
        i++;
    }
    /* Canary gone: the stack overflowed, so all of it was used. */
    if (words[0] != STACK_CANARY) {
        return t->stack_size;
    }
    return t->stack_size - i * (uint32_t)sizeof(uint32_t);
}

/* -------------------------------------------------------------------------
 * Free / recycle
 * ------------------------------------------------------------------------- */

void task_free_thread(tcb_t *t)
{
    if (t == NULL) {
        return;
    }

    uint8_t *stack = NULL;
    uint32_t save = spinlock_irq_acquire(&task_lock);

    /* Remove this thread from the owning process's thread array. */
    pcb_t *proc = find_process_locked(t->pid);
    if (proc != NULL) {
        for (uint32_t i = 0; i < proc->thread_count; i++) {
            if (proc->threads[i] == t) {
                /* Compact: shift remaining pointers down one slot. */
                proc->thread_count--;
                for (uint32_t j = i; j < proc->thread_count; j++) {
                    proc->threads[j] = proc->threads[j + 1];
                }
                proc->threads[proc->thread_count] = NULL;
                break;
            }
        }
    }

    /* Free the TCB slot. */
    for (int i = 0; i < MAX_THREADS; i++) {
        if (&tcb_pool[i] == t) {
            stack = t->stack_base;
            tcb_used[i] = false;
            memset(t, 0, sizeof(tcb_t));
            break;
        }
    }

    /* If the process now owns no threads, free the PCB slot. */
    if (proc != NULL && proc->thread_count == 0) {
        free_process_locked(proc);
    }

    spinlock_irq_release(&task_lock, save);

    /* Return stack memory to the heap outside task_lock (heap_lock is
     * separate; task_lock stays a leaf). */
    if (stack != NULL) {
        kfree(stack);
    }
}

void task_free_process(pcb_t *p)
{
    if (p == NULL) {
        return;
    }
    uint32_t save = spinlock_irq_acquire(&task_lock);
    free_process_locked(p);
    spinlock_irq_release(&task_lock, save);
}

/* -------------------------------------------------------------------------
 * task_kill_thread
 *
 * sched_kill() decides, under the scheduler lock, whether t can be freed
 * now.  A thread that is running (here or on the other core) or waiting on
 * a sync primitive cannot: freeing it would pull the stack out from under
 * a running CPU, or leave a dangling pointer in a waiter list.  Those are
 * freed later by sched_next_thread() or sched_unblock().  A thread holding
 * a kmutex keeps running until it releases the last one, then dies in
 * kmutex_unlock() (sched_kill_point).
 * ------------------------------------------------------------------------- */
void task_kill_thread(tcb_t *t)
{
    if (t != NULL && sched_kill(t)) {
        task_free_thread(t);   /* also frees the PCB when last thread gone */
    }
}

/* -------------------------------------------------------------------------
 * task_kill_process
 *
 * Kill every thread in proc.  The PCB is freed with its last thread, which
 * may be later than this call (see task_kill_thread), so mark it not alive
 * now: owners of WiFi/BT continuous scans are reclaimed on that, and
 * task_create_thread() refuses to add threads to it.
 *
 * Snapshot the thread array first (under task_lock), because
 * task_free_thread() compacts proc->threads[] as it goes and would corrupt
 * a live iteration.
 * ------------------------------------------------------------------------- */
void task_kill_process(pcb_t *proc)
{
    if (proc == NULL) {
        return;
    }

    tcb_t *snapshot[MAX_THREADS];
    uint32_t save = spinlock_irq_acquire(&task_lock);
    proc->alive = false;
    uint32_t count = proc->thread_count;
    for (uint32_t i = 0; i < count; i++) {
        snapshot[i] = proc->threads[i];
    }
    spinlock_irq_release(&task_lock, save);

    for (uint32_t i = 0; i < count; i++) {
        task_kill_thread(snapshot[i]);
    }
}

/* -------------------------------------------------------------------------
 * Counts
 * ------------------------------------------------------------------------- */

int task_thread_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (tcb_used[i]) {
            n++;
        }
    }
    return n;
}

int task_process_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb_used[i]) {
            n++;
        }
    }
    return n;
}
