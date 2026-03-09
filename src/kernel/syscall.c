#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "sync.h"
#include "vfs.h"

#include <stdint.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * syscall_dispatch
 *
 * Each case maps a syscall number to the appropriate kernel function.
 * Arguments are passed as generic uint32_t values and cast as needed.
 *
 * Teaching note: in a system with an MPU this would be the SVC handler,
 * validating pointers before dereferencing them.  Without MPU enforcement
 * we simply trust the caller — a deliberate simplification.
 * ------------------------------------------------------------------------- */
int32_t syscall_dispatch(uint32_t num,
                         uint32_t a0,
                         uint32_t a1,
                         uint32_t a2,
                         uint32_t a3)
{
    (void)a3;   /* suppress unused-parameter warning for now */

    switch ((syscall_num_t)num) {

    /* ------------------------------------------------------------------
     * SYS_SPAWN — create a new process and a single thread within it.
     *
     * a0 = const char *name
     * a1 = void (*entry)(void *)
     * a2 = void *arg
     * Returns the new PID on success, or -1 on failure.
     * ------------------------------------------------------------------ */
    case SYS_SPAWN: {
        const char    *name  = (const char *)a0;
        void         (*entry)(void *) = (void (*)(void *))a1;
        void          *arg   = (void *)a2;

        /* Assign the next available PID (simple counter — good enough). */
        static uint32_t next_pid = 10;   /* 1..9 reserved for kernel */
        uint32_t pid = next_pid++;

        pcb_t *proc = task_create_process(name, pid);
        if (proc == NULL) {
            return -1;
        }
        tcb_t *t = task_create_thread(proc, name, entry, arg,
                                      4 /* default priority */,
                                      DEFAULT_STACK_SIZE);
        if (t == NULL) {
            task_free_process(proc);
            return -1;
        }
        return (int32_t)pid;
    }

    /* ------------------------------------------------------------------
     * SYS_THREAD_CREATE — create a new thread in an existing process.
     *
     * a0 = uint32_t pid  (owning process)
     * a1 = void (*entry)(void *)
     * a2 = void *arg
     * Returns the new TID on success, or -1 on failure.
     * ------------------------------------------------------------------ */
    case SYS_THREAD_CREATE: {
        uint32_t pid  = a0;
        void   (*entry)(void *) = (void (*)(void *))a1;
        void   *arg  = (void *)a2;

        pcb_t *proc = task_find_process(pid);
        if (proc == NULL) {
            return -1;
        }
        tcb_t *t = task_create_thread(proc, "thread", entry, arg,
                                      4 /* default priority */,
                                      DEFAULT_STACK_SIZE);
        if (t == NULL) {
            return -1;
        }
        return (int32_t)t->tid;
    }

    /* ------------------------------------------------------------------
     * SYS_EXIT — terminate the current thread.
     *
     * a0 = int exit_code
     * ------------------------------------------------------------------ */
    case SYS_EXIT: {
        if (current_tcb != NULL) {
            current_tcb->state = THREAD_ZOMBIE;
        }
        /* Update process exit code if it owns no more live threads. */
        if (current_tcb != NULL) {
            pcb_t *proc = task_find_process(current_tcb->pid);
            if (proc != NULL) {
                proc->exit_code = (int)a0;
                /* Check whether all threads in the process are zombies. */
                bool all_dead = true;
                for (uint32_t i = 0; i < proc->thread_count; i++) {
                    tcb_t *t = proc->threads[i];
                    if (t != NULL && t->state != THREAD_ZOMBIE) {
                        all_dead = false;
                        break;
                    }
                }
                if (all_dead) {
                    proc->alive = false;
                }
            }
        }
        sched_yield();
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_YIELD — voluntarily give up the CPU.
     * ------------------------------------------------------------------ */
    case SYS_YIELD: {
        sched_yield();
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_SLEEP — sleep for at least a0 milliseconds.
     * ------------------------------------------------------------------ */
    case SYS_SLEEP: {
        sched_sleep(a0);
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_OPEN — open a file or device.
     *
     * a0 = const char *path
     * a1 = int mode
     * Returns fd >= 0 on success, or -1 on failure.
     * ------------------------------------------------------------------ */
    case SYS_OPEN: {
        const char *path = (const char *)a0;
        int         mode = (int)a1;
        return (int32_t)vfs_open(path, mode);
    }

    /* ------------------------------------------------------------------
     * SYS_READ — read from a file descriptor.
     *
     * a0 = int fd
     * a1 = uint8_t *buf
     * a2 = uint32_t n
     * ------------------------------------------------------------------ */
    case SYS_READ: {
        int      fd  = (int)a0;
        uint8_t *buf = (uint8_t *)a1;
        uint32_t n   = a2;
        return (int32_t)vfs_read(fd, buf, n);
    }

    /* ------------------------------------------------------------------
     * SYS_WRITE — write to a file descriptor.
     *
     * a0 = int fd
     * a1 = const uint8_t *buf
     * a2 = uint32_t n
     * ------------------------------------------------------------------ */
    case SYS_WRITE: {
        int            fd  = (int)a0;
        const uint8_t *buf = (const uint8_t *)a1;
        uint32_t       n   = a2;
        return (int32_t)vfs_write(fd, buf, n);
    }

    /* ------------------------------------------------------------------
     * SYS_CLOSE — close a file descriptor.
     * ------------------------------------------------------------------ */
    case SYS_CLOSE: {
        return (int32_t)vfs_close((int)a0);
    }

    /* ------------------------------------------------------------------
     * SYS_MQ_SEND — send a message to a message queue.
     *
     * a0 = mqueue_t *q
     * a1 = const void *msg
     * ------------------------------------------------------------------ */
    case SYS_MQ_SEND: {
        mqueue_t   *q   = (mqueue_t *)a0;
        const void *msg = (const void *)a1;
        mqueue_send(q, msg);
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_MQ_RECV — receive a message from a message queue.
     *
     * a0 = mqueue_t *q
     * a1 = void *msg_out
     * ------------------------------------------------------------------ */
    case SYS_MQ_RECV: {
        mqueue_t *q       = (mqueue_t *)a0;
        void     *msg_out = (void *)a1;
        mqueue_recv(q, msg_out);
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_MUTEX_LOCK
     *
     * a0 = mutex_t *m
     * ------------------------------------------------------------------ */
    case SYS_MUTEX_LOCK: {
        kmutex_t *m = (kmutex_t *)a0;
        kmutex_lock(m);
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_MUTEX_UNLOCK
     *
     * a0 = mutex_t *m
     * ------------------------------------------------------------------ */
    case SYS_MUTEX_UNLOCK: {
        kmutex_t *m = (kmutex_t *)a0;
        kmutex_unlock(m);
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_GETPID — return the PID of the calling thread's process.
     * ------------------------------------------------------------------ */
    case SYS_GETPID: {
        if (current_tcb == NULL) {
            return 0;
        }
        return (int32_t)current_tcb->pid;
    }

    /* ------------------------------------------------------------------
     * SYS_GETTID — return the TID of the calling thread.
     * ------------------------------------------------------------------ */
    case SYS_GETTID: {
        if (current_tcb == NULL) {
            return 0;
        }
        return (int32_t)current_tcb->tid;
    }

    /* ------------------------------------------------------------------
     * SYS_PS — print a summary of all processes (to stdout / USB console).
     * ------------------------------------------------------------------ */
    case SYS_PS: {
        printf("PID  NAME             THREADS  ALIVE\n");
        printf("---  ---------------  -------  -----\n");
        for (int i = 0; i < MAX_PROCESSES; i++) {
            pcb_t *p = task_get_process_slot(i);
            if (p == NULL) {
                continue;
            }
            printf("%-4u %-16s %-8u %s\n",
                   p->pid, p->name, p->thread_count,
                   p->alive ? "yes" : "no");
        }
        return 0;
    }

    /* ------------------------------------------------------------------
     * SYS_KILL — move a thread to ZOMBIE state.
     *
     * a0 = uint32_t tid
     * ------------------------------------------------------------------ */
    case SYS_KILL: {
        uint32_t tid = a0;
        tcb_t   *t   = task_find_thread(tid);
        if (t == NULL) {
            return -1;
        }
        t->state = THREAD_ZOMBIE;
        return 0;
    }

    default:
        return -1;
    }
}
