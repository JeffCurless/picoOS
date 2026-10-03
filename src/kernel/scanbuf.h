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

#ifndef KERNEL_SCANBUF_H
#define KERNEL_SCANBUF_H

/*
 * scanbuf — triple-buffer index rotation for continuous scan results.
 *
 * The caller owns three result lists (an array of 3) and uses the indices
 * kept here to decide which list plays which role:
 *
 *   fill   being written by the radio callback (IRQ context)
 *   ready  the latest finished window, waiting to be taken
 *   held   owned by the consumer thread until its next take
 *
 * scanbuf_publish() swaps fill <-> ready at the end of a window, and
 * scanbuf_take() swaps ready <-> held when the consumer asks for the next
 * window.  Nothing is copied: the roles move, the lists stay put.  Because
 * the consumer's list is never fill or ready, it can read it with no lock.
 *
 * If the consumer is slow, publish() replaces an untaken ready window with
 * the newer one and counts the loss in `dropped`.
 *
 * scanbuf does no locking of its own.  The caller must serialise publish()
 * and take() against each other and against the writer reading `fill`
 * (wifi.c and bluetooth.c hold a kmutex plus the CYW43 async-context lock).
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  fill;      /* list being written                          */
    uint8_t  ready;     /* latest finished window                      */
    uint8_t  held;      /* list owned by the consumer                  */
    bool     fresh;     /* ready holds a window not yet taken          */
    uint32_t seq;       /* windows published so far                    */
    uint32_t dropped;   /* windows overwritten before they were taken  */
} scanbuf_t;

/* fill = 0, ready = 1, held = 2, nothing fresh, counters zero. */
void scanbuf_init(scanbuf_t *b);

/* End the current window: fill <-> ready, seq++.  If the old ready window
 * was never taken it is lost and dropped++. */
void scanbuf_publish(scanbuf_t *b);

/* If a fresh window is ready, ready <-> held and return true; the new
 * window is then list[b->held].  Returns false if nothing new is ready. */
bool scanbuf_take(scanbuf_t *b);

#endif /* KERNEL_SCANBUF_H */
