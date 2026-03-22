#include "task.h"
#include "sched.h"   /* sched_add_thread, sched_remove_thread */
#include "mem.h"     /* kmalloc / kfree  */

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

/* current_tcb is declared volatile in sched.h and defined here so that the
 * linker has exactly one definition. */
tcb_t * volatile current_tcb = NULL;

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
    current_tcb = NULL;
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
 *   low address (stack_base + STACK_CANARY written here)
 *
 * The PendSV handler loads saved_sp, pops r4-r11, updates PSP, then returns
 * with the thread's saved EXC_RETURN value which causes the hardware to pop r0-r3, r12,
 * lr, pc, xpsr from the PSP stack.
 * ------------------------------------------------------------------------- */
tcb_t *task_create_thread(pcb_t      *proc,
                           const char *name,
                           void      (*entry)(void *),
                           void       *arg,
                           uint8_t     priority,
                           uint32_t    stack_size)
{
    /* Find a free TCB slot. */
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!tcb_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return NULL;   /* pool exhausted */
    }

    tcb_t *t = &tcb_pool[slot];
    tcb_used[slot] = true;

    /* Allocate the stack from the kernel heap. */
    t->stack_base = (uint8_t *)kmalloc(stack_size);
    if (t->stack_base == NULL) {
        tcb_used[slot] = false;
        return NULL;           /* heap exhausted */
    }

    /* Basic fields. */
    t->tid        = next_tid++;
    t->pid        = proc ? proc->pid : 0;
    t->stack_size = stack_size;
    t->priority   = priority;
    t->affinity   = 0;                  /* run on any core */
    t->state      = THREAD_READY;
    t->wake_time_us = 0;
    t->cpu_time_us  = 0;
    t->next       = NULL;

    strncpy(t->name, name ? name : "thread", sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';

    /* Write canary at the very bottom of the stack (lowest address). */
    uint32_t *canary_ptr = (uint32_t *)t->stack_base;
    *canary_ptr = STACK_CANARY;

    /* Build the initial stack frame.
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

    /* Link thread into its owning process. */
    if (proc != NULL && proc->thread_count < MAX_THREADS) {
        proc->threads[proc->thread_count++] = t;
    }

    /* Notify the scheduler so it can add the thread to a ready queue. */
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
    if (current_tcb != NULL) {
        current_tcb->state = THREAD_ZOMBIE;
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
    /* Find a free PCB slot. */
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcb_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return NULL;   /* pool exhausted */
    }

    pcb_t *p = &pcb_pool[slot];
    pcb_used[slot] = true;

    p->pid          = pid;
    p->parent_pid   = 0;
    p->thread_count = 0;

    if (pid == 1u) {
        s_kernel_proc = p;
    }
    p->heap_base    = NULL;
    p->heap_size    = 0;
    p->exit_code    = 0;
    p->alive        = true;

    strncpy(p->name, name ? name : "process", sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';

    strncpy(p->cwd, "/", sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = '\0';

    /* Initialise file descriptor table: -1 means unused. */
    for (int i = 0; i < MAX_FDS; i++) {
        p->fd_table[i] = -1;
    }

    /* Clear thread pointers. */
    for (int i = 0; i < MAX_THREADS; i++) {
        p->threads[i] = NULL;
    }

    return p;
}

/* -------------------------------------------------------------------------
 * Lookup helpers
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
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb_used[i] && pcb_pool[i].pid == pid) {
            return &pcb_pool[i];
        }
    }
    return NULL;
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
 * Free / recycle
 * ------------------------------------------------------------------------- */

void task_free_thread(tcb_t *t)
{
    if (t == NULL) {
        return;
    }

    /* Save the PID now — memset below will zero the TCB. */
    uint32_t pid = t->pid;

    /* Remove this thread from the owning process's thread array. */
    pcb_t *proc = task_find_process(pid);
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
            uint8_t *stack = t->stack_base;
            tcb_used[i] = false;
            memset(t, 0, sizeof(tcb_t));
            kfree(stack);   /* return stack memory to the heap */
            break;
        }
    }

    /* If the process now owns no threads, free the PCB slot. */
    if (proc != NULL && proc->thread_count == 0) {
        task_free_process(proc);
    }
}

void task_free_process(pcb_t *p)
{
    if (p == NULL) {
        return;
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&pcb_pool[i] == p) {
            pcb_used[i] = false;
            memset(p, 0, sizeof(pcb_t));
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * task_kill_process
 *
 * Kill every thread in proc, then free the PCB.
 *
 * Strategy: snapshot the thread array first, because task_free_thread()
 * compacts proc->threads[] as it goes and would corrupt a live iteration.
 * For each thread:
 *   - non-self: ZOMBIE → sched_remove_thread → task_free_thread (immediate)
 *   - self:     ZOMBIE only; sched_next_thread reaps on the next yield, and
 *               that reap will call task_free_thread which auto-frees the PCB
 *               when thread_count reaches zero.
 * ------------------------------------------------------------------------- */
void task_kill_process(pcb_t *proc)
{
    if (proc == NULL) {
        return;
    }

    /* Snapshot: task_free_thread compacts proc->threads[], so we must not
     * iterate it directly. */
    uint32_t count = proc->thread_count;
    tcb_t *snapshot[MAX_THREADS];
    for (uint32_t i = 0; i < count; i++) {
        snapshot[i] = proc->threads[i];
    }

    for (uint32_t i = 0; i < count; i++) {
        tcb_t *t = snapshot[i];
        if (t == NULL) {
            continue;
        }
        t->state = THREAD_ZOMBIE;
        if (t != (tcb_t *)current_tcb) {
            sched_remove_thread(t);
            task_free_thread(t);   /* also frees PCB when last thread gone */
        }
        /* Self-kill: scheduler reaps on next yield via sched_next_thread;
         * task_free_thread called there will free the PCB automatically. */
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
