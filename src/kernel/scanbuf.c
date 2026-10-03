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

#include "scanbuf.h"

void scanbuf_init(scanbuf_t *b)
{
    b->fill    = 0u;
    b->ready   = 1u;
    b->held    = 2u;
    b->fresh   = false;
    b->seq     = 0u;
    b->dropped = 0u;
}

void scanbuf_publish(scanbuf_t *b)
{
    if (b->fresh) b->dropped++;     /* consumer never took the old window */

    uint8_t t = b->fill;
    b->fill   = b->ready;
    b->ready  = t;
    b->fresh  = true;
    b->seq++;
}

bool scanbuf_take(scanbuf_t *b)
{
    if (!b->fresh) return false;

    uint8_t t = b->held;
    b->held   = b->ready;
    b->ready  = t;
    b->fresh  = false;
    return true;
}
