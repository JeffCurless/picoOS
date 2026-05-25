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

/*
 * pi.c — Monte Carlo pi estimation using SMP worker threads
 *
 * Spawns PI_NUM_WORKERS threads split evenly across both cores.  Each worker
 * throws PI_PER_WORKER random darts at a unit square and counts hits inside
 * the inscribed quarter-circle.  The coordinator collects results via a
 * semaphore and computes:
 *
 *   π ≈ 4 × total_hits / PI_TOTAL_SAMPLES
 *
 * Random numbers use a per-worker LCG (no shared state, no lock needed).
 * All arithmetic is integer to avoid software-float overhead on Cortex-M0+.
 */

#include "pi.h"
#include "../kernel/sync.h"
#include "../kernel/syscall.h"
#include "../kernel/task.h"
#include "../kernel/mem.h"
#include "../kernel/arch.h"
#include "../shell/shell.h"

#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

#define PI_NUM_WORKERS     4u
#define PI_TOTAL_SAMPLES   10000000u
#define PI_PER_WORKER      (PI_TOTAL_SAMPLES / PI_NUM_WORKERS)

/*
 * Quarter-circle radius in integer coordinates.
 * x, y are sampled in [0, PI_RADIUS].  A dart hits if x²+y² ≤ PI_RADIUS².
 *
 * Overflow check (uint32_t):
 *   PI_RADIUS² = 32767² = 1,073,676,289
 *   x²+y² max  = 2 × 1,073,676,289 = 2,147,352,578 < 2,147,483,647 (UINT32_MAX/2)  ✓
 */
#define PI_RADIUS          32767u

/* Affinity tables selected at runtime. */
static const int8_t pi_affinity_dual[PI_NUM_WORKERS] = {
    THREAD_AFFINITY_C0, THREAD_AFFINITY_C0,
    THREAD_AFFINITY_C1, THREAD_AFFINITY_C1,
};
static const int8_t pi_affinity_single[PI_NUM_WORKERS] = {
    THREAD_AFFINITY_C0, THREAD_AFFINITY_C0,
    THREAD_AFFINITY_C0, THREAD_AFFINITY_C0,
};

/* -------------------------------------------------------------------------
 * Shared state
 *
 * Static so the coordinator and workers can share without an extra pointer.
 * Running pi_estimate twice concurrently would alias these; for an
 * interactive shell demo that is not a concern.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t seed;      /* per-worker LCG seed (set before thread creation) */
    uint32_t n;         /* number of darts to throw                         */
    uint32_t hits;      /* result written by worker before signalling        */
    int8_t   affinity;  /* core affinity for this worker                    */
} pi_worker_arg_t;

static pi_worker_arg_t pi_args[PI_NUM_WORKERS];
static ksemaphore_t    pi_done;

/* -------------------------------------------------------------------------
 * pi_worker
 *
 * Runs on whichever core its affinity selects.  Uses a private LCG state
 * so no synchronisation is needed during the hot loop.
 * -------------------------------------------------------------------------
 *
 * LCG parameters from Numerical Recipes:
 *   state = state × 1664525 + 1013904223
 * Extract 15 upper bits → x, next step → y, both in [0, 32767].
 * ------------------------------------------------------------------------- */
static void pi_worker(void *arg)
{
    pi_worker_arg_t *a = (pi_worker_arg_t *)arg;

    CURRENT_TCB->affinity = a->affinity;

    uint32_t state  = a->seed;
    uint32_t hits   = 0u;
    const uint32_t r2 = PI_RADIUS * PI_RADIUS;

    for (uint32_t i = 0u; i < a->n; i++) {
        state = state * 1664525u + 1013904223u;
        uint32_t x = (state >> 17) & PI_RADIUS;

        state = state * 1664525u + 1013904223u;
        uint32_t y = (state >> 17) & PI_RADIUS;

        if (x * x + y * y <= r2) {
            hits++;
        }
    }

    a->hits = hits;
    ksemaphore_signal(&pi_done);
    /* Return → thread_exit() → ZOMBIE → scheduler reaps */
}

/* -------------------------------------------------------------------------
 * pi_estimate — coordinator, registered in app_table[]
 *
 * Creates the four worker threads, waits for each to finish, then computes
 * and prints the π estimate and elapsed time.
 * ------------------------------------------------------------------------- */
void pi_estimate(void *arg)
{
    /* Parse the optional argument passed by cmd_run.  The pointer is a
     * heap allocation owned by this thread; free it as soon as we're done
     * reading it so no other code needs to know about the lifetime. */
    bool single_core = false;
    if (arg != NULL) {
        single_core = (strcmp((const char *)arg, "single") == 0);
        kfree(arg);
    }

    const int8_t *affinity = single_core ? pi_affinity_single : pi_affinity_dual;
    const char   *mode_str = single_core ? "single-core (all workers on core 0)"
                                         : "dual-core (workers 0,1→core 0  2,3→core 1)";

    shell_print("[pi] Monte Carlo pi estimation\r\n");
    shell_print("[pi] mode    : %s\r\n", mode_str);
    shell_print("[pi] samples : %u  (%u workers × %u each)\r\n",
                (unsigned)PI_TOTAL_SAMPLES,
                (unsigned)PI_NUM_WORKERS,
                (unsigned)PI_PER_WORKER);

    ksemaphore_init(&pi_done, 0);

    pcb_t *proc = task_find_process((uint32_t)sys_getpid());

    /* Distinct seeds so each worker explores a different region of the LCG
     * sequence.  Arbitrary constants — reproducibility over independence. */
    static const uint32_t seeds[PI_NUM_WORKERS] = {
        0xDEAD1337u, 0xCAFEBABEu, 0x8BADF00Du, 0xFEEDFACEu,
    };

    uint64_t t0 = time_us_64();

    for (uint32_t i = 0u; i < PI_NUM_WORKERS; i++) {
        pi_args[i].seed     = seeds[i];
        pi_args[i].n        = PI_PER_WORKER;
        pi_args[i].hits     = 0u;
        pi_args[i].affinity = affinity[i];
        task_create_thread(proc, "pi-worker", pi_worker, &pi_args[i],
                           4u, DEFAULT_STACK_SIZE);
    }

    /* Wait for all workers — each signals pi_done once on completion. */
    for (uint32_t i = 0u; i < PI_NUM_WORKERS; i++) {
        ksemaphore_wait(&pi_done);
        shell_print("[pi] %u/%u workers done\r\n",
                    (unsigned)(i + 1u), (unsigned)PI_NUM_WORKERS);
    }

    uint64_t elapsed_us = time_us_64() - t0;

    /* Sum hits and report per-worker breakdown. */
    uint32_t total_hits = 0u;
    for (uint32_t i = 0u; i < PI_NUM_WORKERS; i++) {
        shell_print("[pi]   worker %u (core %d): %u hits\r\n",
                    (unsigned)i, (int)affinity[i],
                    (unsigned)pi_args[i].hits);
        total_hits += pi_args[i].hits;
    }

    /*
     * π ≈ 4 × total_hits / PI_TOTAL_SAMPLES
     *
     * Fraction uses uint64_t intermediate to avoid overflow:
     *   remainder × 10000 can reach ~9.99 × 10⁹ > UINT32_MAX.
     */
    uint32_t pi_scaled = 4u * total_hits;
    uint32_t pi_int    = pi_scaled / PI_TOTAL_SAMPLES;
    uint32_t pi_frac   = (uint32_t)(
                             ((uint64_t)(pi_scaled % PI_TOTAL_SAMPLES) * 10000u)
                             / PI_TOTAL_SAMPLES);

    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000u);

    shell_print("[pi] ----------------------------------------\r\n");
    shell_print("[pi] total hits : %u / %u\r\n",
                (unsigned)total_hits, (unsigned)PI_TOTAL_SAMPLES);
    shell_print("[pi] pi estimate: %u.%04u\r\n", (unsigned)pi_int, (unsigned)pi_frac);
    shell_print("[pi] true value : 3.1416\r\n");
    shell_print("[pi] elapsed    : %u ms\r\n", (unsigned)elapsed_ms);
}
