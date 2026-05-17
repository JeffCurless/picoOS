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
 * in a product or service whose value derived substantially from the software
 * without prior written permission from the copyright holder.
 */

/*
 * tests/mem/test_mem.c — host-native tests for the picoOS boundary-tag heap.
 *
 * Build and run from the tests/ directory:
 *   cmake -B build && make -C build
 *   ./build/test_mem
 *
 * Or via CTest:
 *   cmake -B build && make -C build && ctest --test-dir build
 *
 * These tests compile mem.c directly against the host toolchain — no Pico SDK
 * or hardware required.  kmem_init() may be called multiple times to reset the
 * heap between tests.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "../framework.h"

/* -------------------------------------------------------------------------
 * Helpers shared by all tests
 * ------------------------------------------------------------------------- */

/*
 * Baseline stats captured immediately after kmem_init() in each test.
 * Used to verify the heap is fully restored once all allocations are freed.
 */
static uint32_t _bl_free;
static uint32_t _bl_largest;

static void capture_baseline(void)
{
    kmem_stats(NULL, &_bl_free, &_bl_largest);
}

static bool heap_is_restored(void)
{
    uint32_t f, l;
    kmem_stats(NULL, &f, &l);
    return (f == _bl_free) && (l == _bl_largest);
}

static void fill_pattern(void *buf, size_t size, uint8_t val)
{
    memset(buf, val, size);
}

static bool verify_pattern(const void *buf, size_t size, uint8_t val)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != val) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * test_zero_size
 * kmalloc(0) must return NULL — explicitly required by the allocator spec.
 * ------------------------------------------------------------------------- */
static void test_zero_size(void)
{
    BEGIN_TEST(zero_size_returns_null);
    kmem_init();

    void *p = kmalloc(0);
    CHECK(p == NULL, "kmalloc(0) must return NULL");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_null_free
 * kfree(NULL) must be a no-op (no crash, no state change).
 * ------------------------------------------------------------------------- */
static void test_null_free(void)
{
    BEGIN_TEST(null_free_is_safe);
    kmem_init();
    capture_baseline();

    kfree(NULL);
    CHECK(heap_is_restored(), "kfree(NULL) must not alter heap state");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_single_byte
 * The smallest legal allocation (1 byte) must succeed, be writable, and the
 * heap must be fully restored once it is freed.
 * ------------------------------------------------------------------------- */
static void test_single_byte(void)
{
    BEGIN_TEST(single_byte_allocation);
    kmem_init();
    capture_baseline();

    uint8_t *p = (uint8_t *)kmalloc(1);
    CHECK(p != NULL, "kmalloc(1) must succeed");
    if (p) {
        *p = 0xAB;
        CHECK(*p == 0xAB, "single byte must be writable and readable");
        kfree(p);
    }
    CHECK(heap_is_restored(), "heap must be restored after freeing 1-byte allocation");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_pointer_alignment
 * Every pointer returned by kmalloc must be 8-byte aligned, regardless of
 * the requested size.
 * ------------------------------------------------------------------------- */
static void test_pointer_alignment(void)
{
    BEGIN_TEST(pointer_alignment_8_bytes);
    kmem_init();

    static const size_t sizes[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17,
        23, 24, 25, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 511, 512
    };
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
    void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

    bool all_aligned = true;
    for (int i = 0; i < n; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i] && ((uintptr_t)ptrs[i] & 7u) != 0) {
            printf("    pointer for size %zu is not 8-byte aligned: %p\n",
                   sizes[i], ptrs[i]);
            all_aligned = false;
        }
    }
    CHECK(all_aligned, "all returned pointers must be 8-byte aligned");

    for (int i = 0; i < n; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_over_heap_size
 * A request larger than the entire heap must return NULL without corrupting
 * the allocator state.
 * ------------------------------------------------------------------------- */
static void test_over_heap_size(void)
{
    BEGIN_TEST(over_heap_size_returns_null);
    kmem_init();
    capture_baseline();

    void *p = kmalloc((size_t)HEAP_SIZE + 1u);
    CHECK(p == NULL, "kmalloc(HEAP_SIZE+1) must return NULL");
    CHECK(heap_is_restored(), "heap must be unmodified after failed allocation");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_max_single_allocation
 * The heap must be able to satisfy a single allocation equal to the full
 * initial free payload.  After freeing, the heap must be identical to its
 * initial state (verifying that the whole-heap block coalesces correctly).
 * ------------------------------------------------------------------------- */
static void test_max_single_allocation(void)
{
    BEGIN_TEST(max_single_allocation);
    kmem_init();
    capture_baseline();

    void *p = kmalloc(_bl_largest);
    CHECK(p != NULL, "allocation of full initial free payload must succeed");
    if (p) {
        /* Write across the entire allocation to catch any off-by-one in sizing. */
        fill_pattern(p, _bl_largest, 0xCC);
        CHECK(verify_pattern(p, _bl_largest, 0xCC),
              "entire max allocation must be writable and readable");
        kfree(p);
    }
    CHECK(heap_is_restored(), "heap must be restored after freeing max-sized block");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_heap_exhaustion
 * Fill the heap with fixed-size blocks until kmalloc returns NULL.
 * Verify that:
 *   - at least one block was allocated,
 *   - the very next allocation after the heap is full returns NULL,
 *   - freeing all blocks completely restores the heap.
 * ------------------------------------------------------------------------- */
#define MAX_PTRS 4096

static void test_heap_exhaustion(void)
{
    BEGIN_TEST(heap_exhaustion_and_restore);
    kmem_init();
    capture_baseline();

    void  *ptrs[MAX_PTRS];
    int    count = 0;

    while (count < MAX_PTRS) {
        void *p = kmalloc(64);
        if (!p) break;
        ptrs[count++] = p;
    }

    CHECK(count > 0, "must succeed on at least one 64-byte allocation");

    void *overflow = kmalloc(64);
    CHECK(overflow == NULL, "allocation after heap exhaustion must return NULL");

    for (int i = 0; i < count; i++) kfree(ptrs[i]);

    CHECK(heap_is_restored(),
          "heap free bytes and max contiguous block must match initial values");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_write_patterns
 * For a range of allocation sizes, fill each block with a unique byte
 * pattern.  After all blocks are live, verify no block has been corrupted
 * by adjacent allocations.  Then free all blocks and verify heap restore.
 * ------------------------------------------------------------------------- */
static void test_write_patterns(void)
{
    BEGIN_TEST(write_patterns_no_corruption);
    kmem_init();
    capture_baseline();

    static const size_t alloc_sizes[] = {
        8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024
    };
    const int n = (int)(sizeof(alloc_sizes) / sizeof(alloc_sizes[0]));
    void    *ptrs[sizeof(alloc_sizes) / sizeof(alloc_sizes[0])];

    for (int i = 0; i < n; i++) {
        ptrs[i] = kmalloc(alloc_sizes[i]);
        CHECK(ptrs[i] != NULL, "allocation must succeed for each preset size");
        if (ptrs[i]) fill_pattern(ptrs[i], alloc_sizes[i], (uint8_t)(i + 1));
    }

    bool all_intact = true;
    for (int i = 0; i < n; i++) {
        if (ptrs[i] && !verify_pattern(ptrs[i], alloc_sizes[i], (uint8_t)(i + 1))) {
            printf("    pattern mismatch on block %d (size %zu)\n", i, alloc_sizes[i]);
            all_intact = false;
        }
    }
    CHECK(all_intact, "fill patterns must be intact across all live allocations");

    for (int i = 0; i < n; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }
    CHECK(heap_is_restored(), "heap must be restored after freeing all pattern blocks");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_random_sizes
 * Allocate blocks of random sizes (1–1023 bytes), record fill patterns,
 * verify all patterns are intact, free all, verify heap restore.
 * A fixed seed makes the test deterministic and reproducible.
 * ------------------------------------------------------------------------- */
static void test_random_sizes(void)
{
    BEGIN_TEST(random_size_allocations);
    kmem_init();
    capture_baseline();

    srand(0x5EED);

    void   *ptrs[512];
    size_t  sizes[512];
    uint8_t pats[512];
    int     count = 0;

    for (int i = 0; i < 512; i++) {
        size_t sz = (size_t)(rand() % 1023) + 1;
        void  *p  = kmalloc(sz);
        if (!p) break;

        uint8_t pat = (uint8_t)(rand() & 0xFF);
        fill_pattern(p, sz, pat);
        ptrs[count]  = p;
        sizes[count] = sz;
        pats[count]  = pat;
        count++;
    }
    CHECK(count > 0, "at least one random-size allocation must succeed");

    bool all_ok = true;
    for (int i = 0; i < count; i++) {
        if (!verify_pattern(ptrs[i], sizes[i], pats[i])) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok, "all fill patterns must be intact after random-size allocations");

    for (int i = 0; i < count; i++) kfree(ptrs[i]);
    CHECK(heap_is_restored(),
          "heap must be fully restored after all random-size blocks freed");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_fragmentation
 * Fill the heap with 8-byte blocks, then free every odd-indexed block.  All
 * odd-indexed blocks carry exactly 8-byte payloads (the no-split terminal
 * block always lands at an even index), leaving alternating 8-byte holes that
 * cannot coalesce.  Any allocation larger than 8 bytes must then fail,
 * demonstrating external fragmentation.  Freeing all remaining blocks must
 * fully restore the heap.
 * ------------------------------------------------------------------------- */
static void test_fragmentation(void)
{
    BEGIN_TEST(external_fragmentation);
    kmem_init();
    capture_baseline();

    /*
     * Fill the heap completely with minimum-overhead 8-byte blocks so there
     * is no large trailing free region that would mask the fragmentation.
     */
    void *fill[MAX_PTRS];
    int   nfill = 0;
    while (nfill < MAX_PTRS) {
        void *p = kmalloc(8);
        if (!p) break;
        fill[nfill++] = p;
    }
    CHECK(nfill > 1, "must fill heap with at least 2 blocks");

    /*
     * Free only odd-indexed blocks.  All odd-indexed blocks have exactly
     * 8-byte payloads (the no-split final block always lands at an even index
     * for this heap / block-size combination).  This leaves alternating 8-byte
     * holes, each surrounded by still-allocated blocks on both sides.
     */
    for (int i = 1; i < nfill; i += 2) {
        kfree(fill[i]);
        fill[i] = NULL;
    }

    /* Every free hole is exactly 8 bytes.  A 16-byte allocation cannot fit
     * in any single hole — the heap is externally fragmented. */
    void *should_fail = kmalloc(16);
    CHECK(should_fail == NULL,
          "16-byte allocation into an 8-byte-fragmented heap must fail");
    if (should_fail) kfree(should_fail);  /* keep heap consistent if check fails */

    /* Free all remaining (even-indexed) blocks. */
    for (int i = 0; i < nfill; i++) {
        if (fill[i]) {
            kfree(fill[i]);
            fill[i] = NULL;
        }
    }
    CHECK(heap_is_restored(),
          "heap must be fully restored once all fragments are freed");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_reverse_free_order
 * Allocate a run of same-sized blocks, then free them in reverse order.
 * Freeing in reverse specifically exercises the backward-coalescing path
 * (the O(n) predecessor walk).  The heap must be fully restored.
 * ------------------------------------------------------------------------- */
static void test_reverse_free_order(void)
{
    BEGIN_TEST(reverse_free_order_coalescing);
    kmem_init();
    capture_baseline();

    void *ptrs[64];
    int   count = 0;

    for (int i = 0; i < 64; i++) {
        void *p = kmalloc(128);
        if (!p) break;
        ptrs[count++] = p;
    }
    CHECK(count > 0, "must allocate at least one block");

    for (int i = count - 1; i >= 0; i--) kfree(ptrs[i]);

    CHECK(heap_is_restored(),
          "heap must be restored after reverse-order frees (backward coalescing)");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_repeated_cycles
 * Allocate and immediately free the same-size block 1 000 times.  The heap
 * must return to its initial state each cycle; any failure to coalesce would
 * accumulate fragmentation and eventually cause allocations to fail.
 * ------------------------------------------------------------------------- */
static void test_repeated_cycles(void)
{
    BEGIN_TEST(repeated_alloc_free_cycles);
    kmem_init();
    capture_baseline();

    bool all_ok = true;
    for (int i = 0; i < 1000; i++) {
        void *p = kmalloc(256);
        if (!p) { all_ok = false; break; }
        kfree(p);
    }
    CHECK(all_ok, "kmalloc(256) must succeed on every one of 1000 repeated cycles");
    CHECK(heap_is_restored(),
          "heap must be in initial state after 1000 alloc/free cycles");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_interleaved_ops
 * Allocate five blocks of different sizes, then free them in a non-sequential
 * order to exercise both forward and backward coalescing in arbitrary
 * combinations.
 * ------------------------------------------------------------------------- */
static void test_interleaved_ops(void)
{
    BEGIN_TEST(interleaved_alloc_free_mixed_sizes);
    kmem_init();
    capture_baseline();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(32);
    void *d = kmalloc(512);
    void *e = kmalloc(16);

    CHECK(a && b && c && d && e, "all five allocations must succeed");

    /* Non-sequential free order: middle, first, last, second, fourth. */
    kfree(c);
    kfree(a);
    kfree(e);
    kfree(b);
    kfree(d);

    CHECK(heap_is_restored(),
          "heap must be restored after out-of-order frees of mixed-size blocks");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * test_boundary_sizes
 * Allocate and free each size that straddles an 8-byte alignment boundary
 * (e.g. 7, 8, 9).  Each allocation must be aligned, writable to its full
 * requested size, and the heap must be restored on free.
 * ------------------------------------------------------------------------- */
static void test_boundary_sizes(void)
{
    BEGIN_TEST(alignment_boundary_sizes);

    static const size_t bsizes[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9,
        13, 14, 15, 16, 17,
        23, 24, 25,
        31, 32, 33,
        63, 64, 65,
        127, 128, 129,
        255, 256, 257,
        511, 512, 513,
    };
    const int n = (int)(sizeof(bsizes) / sizeof(bsizes[0]));
    bool all_ok = true;

    for (int i = 0; i < n; i++) {
        kmem_init();
        uint32_t f0, l0;
        kmem_stats(NULL, &f0, &l0);

        size_t sz = bsizes[i];
        void  *p  = kmalloc(sz);
        if (!p) { all_ok = false; continue; }

        if ((uintptr_t)p & 7u) {
            printf("    misaligned pointer for size %zu: %p\n", sz, p);
            all_ok = false;
        }

        /* Write to the entire requested size — no overflow detected here means
         * the allocator rounded up correctly without exposing heap internals. */
        memset(p, (int)(sz & 0xFF), sz);
        kfree(p);

        uint32_t f1, l1;
        kmem_stats(NULL, &f1, &l1);
        if (f1 != f0 || l1 != l0) {
            printf("    heap not restored after size %zu\n", sz);
            all_ok = false;
        }
    }
    CHECK(all_ok,
          "all boundary sizes must allocate, align, be writable, and restore the heap");

    END_TEST();
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("picoOS kernel heap allocator — unit tests\n");
    printf("==========================================\n\n");

    test_zero_size();
    test_null_free();
    test_single_byte();
    test_pointer_alignment();
    test_over_heap_size();
    test_max_single_allocation();
    test_heap_exhaustion();
    test_write_patterns();
    test_random_sizes();
    test_fragmentation();
    test_reverse_free_order();
    test_repeated_cycles();
    test_interleaved_ops();
    test_boundary_sizes();

    SUMMARY();
}
