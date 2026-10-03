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
 * tests/scanbuf/test_scanbuf.c — host-native tests for the scan triple buffer.
 *
 * Covers: initial roles, take with nothing ready, publish/take round trips,
 * dropped-window counting, and the invariant that fill, ready and held are
 * always three distinct lists (so the consumer never reads a list the radio
 * is writing).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "scanbuf.h"
#include "../framework.h"

static bool distinct(const scanbuf_t *b)
{
    return b->fill < 3u && b->ready < 3u && b->held < 3u &&
           b->fill != b->ready && b->fill != b->held && b->ready != b->held;
}

static void test_init(void)
{
    BEGIN_TEST(init_roles);
    scanbuf_t b;
    scanbuf_init(&b);
    CHECK(b.fill == 0u && b.ready == 1u && b.held == 2u, "initial roles");
    CHECK(!b.fresh, "nothing fresh");
    CHECK(b.seq == 0u && b.dropped == 0u, "counters zero");
    END_TEST();
}

static void test_take_empty(void)
{
    BEGIN_TEST(take_with_nothing_ready);
    scanbuf_t b;
    scanbuf_init(&b);
    CHECK(!scanbuf_take(&b), "take returns false");
    CHECK(b.held == 2u && b.ready == 1u, "roles unchanged");
    END_TEST();
}

static void test_publish_take(void)
{
    BEGIN_TEST(publish_then_take_hands_over_filled_list);
    scanbuf_t b;
    scanbuf_init(&b);
    uint8_t filled = b.fill;
    scanbuf_publish(&b);
    CHECK(b.ready == filled, "filled list becomes ready");
    CHECK(b.fresh && b.seq == 1u, "fresh, seq 1");
    CHECK(scanbuf_take(&b), "take succeeds");
    CHECK(b.held == filled, "consumer now holds the filled list");
    CHECK(!b.fresh, "no longer fresh");
    CHECK(!scanbuf_take(&b), "second take with nothing new fails");
    CHECK(b.held == filled, "held unchanged by failed take");
    CHECK(distinct(&b), "roles distinct");
    END_TEST();
}

static void test_dropped(void)
{
    BEGIN_TEST(slow_consumer_counts_dropped_windows);
    scanbuf_t b;
    scanbuf_init(&b);
    scanbuf_publish(&b);
    CHECK(b.dropped == 0u, "first publish drops nothing");
    uint8_t newest = b.fill;
    scanbuf_publish(&b);
    CHECK(b.dropped == 1u, "untaken window dropped");
    CHECK(b.ready == newest, "newest window is ready");
    scanbuf_publish(&b);
    CHECK(b.dropped == 2u && b.seq == 3u, "dropped 2, seq 3");
    CHECK(scanbuf_take(&b), "take after drops succeeds");
    scanbuf_publish(&b);
    CHECK(b.dropped == 2u, "publish after take drops nothing");
    END_TEST();
}

static void test_invariant(void)
{
    BEGIN_TEST(roles_stay_distinct_over_random_sequence);
    scanbuf_t b;
    scanbuf_init(&b);
    uint32_t x = 12345u;
    bool ok = true;
    uint8_t last_ready_pub = 0xFFu;
    for (int i = 0; i < 10000; i++) {
        x = x * 1103515245u + 12345u;
        if ((x >> 16) & 1u) {
            last_ready_pub = b.fill;
            scanbuf_publish(&b);
        } else {
            bool fresh = b.fresh;
            bool took  = scanbuf_take(&b);
            if (took != fresh) ok = false;
            if (took && b.held != last_ready_pub) ok = false;
        }
        if (!distinct(&b)) ok = false;
    }
    CHECK(ok, "roles distinct and take returns the latest published list");
    END_TEST();
}

int main(void)
{
    printf("scanbuf tests\n");
    test_init();
    test_take_empty();
    test_publish_take();
    test_dropped();
    test_invariant();
    SUMMARY();
}
