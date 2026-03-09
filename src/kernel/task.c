#include "task.h"
#include "sched.h"   /* sched_add_thread */

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
_Static_assert(offsetof(tcb_t, saved_sp) == 16,
               "saved_sp offset mismatch: update the #16 offset in sched_asm.S");
#endif

/* -------------------------------------------------------------------------
 * Static pools
 *
 * All memory for TCBs, PCBs, and thread stacks comes from these arrays so
 * the linker can account for every byte at build time.  No dynamic
 * allocation is used in the task manager itself.
 * ------------------------------------------------------------------------- */

static tcb_t tcb_pool[MAX_THREADS];
static pcb_t pcb_pool[MAX_PROCESSES];

/*
 * One flat stack pool.  We use SERVICE_STACK_SIZE for every slot to keep the
 * code simple (the teaching point is the concept, not the byte-counting).
 * A future optimisation would use per-thread size hints to pack stacks.
 */
static uint8_t stack_pool[MAX_THREADS][SERVICE_STACK_SIZE]
    __attribute__((aligned(8)));

/* in-use flags */
static bool tcb_used[MAX_THREADS];
static bool pcb_used[MAX_PROCESSES];

/* monotonically increasing ID counters */
static uint32_t next_tid = 1;

/* current_tcb is declared volatile in sched.h and defined here so that the
 * linker has exactly one definition. */
tcb_t * volatile current_tcb = NULL;

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
    memset(stack_pool, 0, sizeof(stack_pool));

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
 * with EXC_RETURN=0xFFFFFFFD which causes the hardware to pop r0-r3, r12,
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

    /* Basic fields. */
    t->tid        = next_tid++;
    t->pid        = proc ? proc->pid : 0;
    t->stack_base = stack_pool[slot];
    t->stack_size = SERVICE_STACK_SIZE;  /* actual allocated size */
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
     *   stack_top = stack_base + SERVICE_STACK_SIZE
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
    uint32_t *stack_top = (uint32_t *)(t->stack_base + SERVICE_STACK_SIZE);

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
    t->saved_sp = &stack_top[-16];

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
    /* Mark the current thread as a zombie so the scheduler skips it.
     * In a more complete kernel this would notify the parent process. */
    if (current_tcb != NULL) {
        current_tcb->state = THREAD_ZOMBIE;
    }
    /* Yield forever; the scheduler will never schedule a ZOMBIE thread. */
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
    for (int i = 0; i < MAX_THREADS; i++) {
        if (&tcb_pool[i] == t) {
            tcb_used[i] = false;
            memset(t, 0, sizeof(tcb_t));
            return;
        }
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
