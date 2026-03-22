#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Compile-time constants
 * ------------------------------------------------------------------------- */

/*
 * Thread and process pool sizes.
 *
 * Memory budget note (RP2040 has 264 KB SRAM total):
 *   thread stacks (dynamic, from kernel heap)    =        variable
 *   kernel heap (mem.c HEAP_SIZE)                =             64 KB
 *   filesystem RAM buffer (fs.c)                 =             16 KB
 *   TCB/PCB pools + code + SDK overhead          ~             50 KB
 *   ---------------------------------------------------------------
 *   Total static footprint                       ~            130 KB  (OK)
 *
 * Thread stacks are allocated dynamically from the kernel heap via kmalloc
 * and freed when the thread exits.  Pass the appropriate SIZE constant to
 * task_create_thread() based on the thread's call-chain depth.
 *
 * Increase MAX_THREADS carefully — each additional live thread consumes at
 * least DEFAULT_STACK_SIZE bytes of heap at runtime.
 */
#define MAX_THREADS        16
#define MAX_PROCESSES      8
#define MAX_FDS            16
#define MAX_OPEN_FILES     8
#define DEFAULT_STACK_SIZE 2048    /* general-purpose threads       */
#define DEEP_STACK_SIZE    3072    /* threads with deep call chains */
#define IDLE_STACK_SIZE     512    /* idle thread                   */
#define STACK_CANARY       0xDEADBEEFu

/* -------------------------------------------------------------------------
 * Thread states
 * ------------------------------------------------------------------------- */

typedef enum {
    THREAD_NEW,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_ZOMBIE
} thread_state_t;

/* -------------------------------------------------------------------------
 * Thread Control Block (TCB)
 *
 * Field order is fixed so that sched_asm.S can use hard-coded offsets:
 *   offset  0 : tid        (4 bytes)
 *   offset  4 : pid        (4 bytes)
 *   offset  8 : stack_base (4 bytes — pointer)
 *   offset 12 : stack_size (4 bytes)
 *   offset 16 : saved_sp   (4 bytes — pointer)   <-- used by PendSV handler
 *   offset 20 : exc_return (4 bytes)              <-- EXC_RETURN for PendSV
 *
 * exc_return tracks the ARM EXC_RETURN value for this thread.  On Cortex-M33
 * (RP2350) the FPU is present; if a thread uses FP instructions the hardware
 * saves an extended exception frame (26 words, LR=0xFFFFFFED) instead of the
 * basic frame (8 words, LR=0xFFFFFFFD).  Storing the correct EXC_RETURN per
 * thread lets PendSV return with the right value, keeping PSP aligned.
 * On Cortex-M0+ (RP2040) there is no FPU so exc_return is always 0xFFFFFFFD.
 *
 * Do NOT reorder these first six fields without updating sched_asm.S.
 * ------------------------------------------------------------------------- */

typedef struct tcb {
    uint32_t        tid;            /* Unique thread ID                      */
    uint32_t        pid;            /* PID of the owning process             */
    uint8_t        *stack_base;     /* Lowest byte of the thread's stack     */
    uint32_t        stack_size;     /* Stack size in bytes                   */
    uint32_t       *saved_sp;       /* Saved stack pointer (context switch)  */
    uint32_t        exc_return;     /* EXC_RETURN used by PendSV on restore  */

    uint8_t         priority;       /* Scheduling priority: 0 = highest      */
    uint8_t         affinity;       /* 0 = any, 1 = core 0, 2 = core 1      */
    thread_state_t  state;          /* Current lifecycle state               */
    uint64_t        wake_time_us;   /* Absolute wake time (us) when sleeping */
    uint64_t        cpu_time_us;    /* Accumulated CPU time in microseconds  */
    char            name[16];       /* Human-readable name (NUL-terminated)  */
    struct tcb     *next;           /* Intrusive linked-list pointer         */
} tcb_t;

/* -------------------------------------------------------------------------
 * Process Control Block (PCB)
 * ------------------------------------------------------------------------- */

typedef struct pcb {
    uint32_t  pid;                      /* Unique process ID                 */
    uint32_t  parent_pid;               /* Parent process ID (0 = none)      */
    char      name[16];                 /* Human-readable process name       */

    tcb_t    *threads[MAX_THREADS];     /* Threads owned by this process     */
    uint32_t  thread_count;             /* Number of live threads            */

    int       fd_table[MAX_FDS];        /* Open file descriptors (-1 = free) */
    char      cwd[64];                  /* Current working directory         */

    uint8_t  *heap_base;                /* Base of process heap region       */
    uint32_t  heap_size;                /* Size of process heap in bytes     */

    int       exit_code;                /* Exit code (valid when !alive)     */
    bool      alive;                    /* False once the process has exited */
} pcb_t;

/* -------------------------------------------------------------------------
 * Global: the TCB currently executing on Core 0.
 * There is a single definition (in task.c).  Core 1 idles in __wfi() and
 * does not maintain a separate current_tcb — a full SMP implementation
 * would need per-core current pointers.
 * ------------------------------------------------------------------------- */

extern tcb_t * volatile current_tcb;

/* -------------------------------------------------------------------------
 * Task manager API
 * ------------------------------------------------------------------------- */

void     task_init(void);

tcb_t   *task_create_thread(pcb_t      *proc,
                             const char *name,
                             void      (*entry)(void *),
                             void       *arg,
                             uint8_t     priority,
                             uint32_t    stack_size);

pcb_t   *task_create_process(const char *name, uint32_t pid);

tcb_t   *task_find_thread(uint32_t tid);
pcb_t   *task_find_process(uint32_t pid);

void     task_free_thread(tcb_t *t);
void     task_free_process(pcb_t *p);

/*
 * task_kill_process — kill every thread owned by proc and free the PCB.
 *                     Threads other than the caller are removed from the
 *                     scheduler and freed immediately.  If the calling thread
 *                     belongs to proc (self-kill), it is marked ZOMBIE and
 *                     the scheduler reaps it on the next yield.
 */
void     task_kill_process(pcb_t *p);

int      task_thread_count(void);
int      task_process_count(void);

/* Iterate over every allocated TCB/PCB slot (used by shell 'ps'/'threads').
 * Returns the slot pointer or NULL when idx is out of range.              */
tcb_t   *task_get_thread_slot(int idx);
pcb_t   *task_get_process_slot(int idx);

/* Returns a pointer to the kernel process (PID 1), or NULL if not yet
 * created.  Used by kernel modules that need to spawn threads in the
 * kernel process (e.g. wifi-poll, future background services).           */
pcb_t   *task_get_kernel_proc(void);

#endif /* KERNEL_TASK_H */
