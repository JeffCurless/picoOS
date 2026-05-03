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

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * System call numbers
 *
 * These are the only way user (application) code should request kernel
 * services.  Keeping the list small makes it easy to audit.
 * ------------------------------------------------------------------------- */
typedef enum {
    SYS_SPAWN         = 0,
    SYS_THREAD_CREATE = 1,
    SYS_EXIT          = 2,
    SYS_YIELD         = 3,
    SYS_SLEEP         = 4,
    SYS_OPEN          = 5,
    SYS_READ          = 6,
    SYS_WRITE         = 7,
    SYS_CLOSE         = 8,
    SYS_MQ_SEND       = 9,
    SYS_MQ_RECV       = 10,
    SYS_MUTEX_LOCK    = 11,
    SYS_MUTEX_UNLOCK  = 12,
    SYS_GETPID        = 13,
    SYS_GETTID        = 14,
    SYS_PS            = 15,
    SYS_KILL          = 16
} syscall_num_t;

/* -------------------------------------------------------------------------
 * syscall_dispatch — the central dispatch function.
 *
 * In a production kernel this would be reached via an SVC instruction.
 * For clarity we call it directly as a regular C function, which is
 * equivalent on a system without MPU enforcement.
 *
 * Returns 0 on success, or a negative errno-style code on failure.
 * ------------------------------------------------------------------------- */
int32_t syscall_dispatch(uint32_t num,
                         uint32_t a0,
                         uint32_t a1,
                         uint32_t a2,
                         uint32_t a3);

/* -------------------------------------------------------------------------
 * Inline helper wrappers
 *
 * Application code should call these instead of syscall_dispatch directly.
 * ------------------------------------------------------------------------- */

static inline void sys_yield(void)
{
    syscall_dispatch(SYS_YIELD, 0, 0, 0, 0);
}

static inline void sys_sleep(uint32_t ms)
{
    syscall_dispatch(SYS_SLEEP, ms, 0, 0, 0);
}

static inline void sys_exit(int code)
{
    syscall_dispatch(SYS_EXIT, (uint32_t)code, 0, 0, 0);
}

static inline int sys_getpid(void)
{
    return (int)syscall_dispatch(SYS_GETPID, 0, 0, 0, 0);
}

static inline int sys_gettid(void)
{
    return (int)syscall_dispatch(SYS_GETTID, 0, 0, 0, 0);
}

#endif /* KERNEL_SYSCALL_H */
