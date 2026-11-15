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

#ifndef KERNEL_MEM_H
#define KERNEL_MEM_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Kernel heap size
 *
 * Thread stacks are now allocated dynamically from this heap (kmalloc in
 * task_create_thread, kfree in task_free_thread).  The previous 64 KB static
 * stack_pool has been removed; this heap absorbs that budget while also
 * serving kernel object allocations.  Net SRAM saving: ~32 KB.
 * ------------------------------------------------------------------------- */
/*
 * Kernel heap size.
 *
 * Holds every thread stack (MAX_THREADS x DEFAULT_STACK_SIZE = 64 KB at
 * peak with 32 threads) plus kernel object allocations.  src/CMakeLists.txt
 * sets it per variant (PICOOS_HEAP_KB): 128 KB on RP2350, 96 KB on RP2040,
 * 64 KB on pico_w + Display Pack 2, where the framebuffer and the WiFi/BT
 * stack leave little SRAM.  The default here is for builds that do not go
 * through that file (the host unit tests).
 */
#ifndef HEAP_SIZE
#define HEAP_SIZE (64u * 1024u)
#endif

/* -------------------------------------------------------------------------
 * Memory manager API
 * ------------------------------------------------------------------------- */

/*
 * kmem_init — initialise the kernel heap.
 *             Must be called once before any kmalloc/kfree calls.
 *             Prefixed with 'k' to avoid collision with lwIP's mem_init.
 */
void kmem_init(void);

/*
 * kmalloc — allocate at least `size` bytes from the kernel heap.
 *           Returns a pointer to the allocation, or NULL on failure.
 *           The returned memory is 8-byte aligned.
 */
void *kmalloc(size_t size);

/*
 * kfree — release a block previously returned by kmalloc.
 *         Adjacent free blocks are coalesced immediately.
 */
void kfree(void *ptr);

/*
 * kmem_stats — fill in heap usage statistics.
 *   *used    — total bytes currently allocated (including headers)
 *   *free    — total free bytes (including headers of free blocks)
 *   *largest — size of the largest single contiguous free block (payload only)
 */
void kmem_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *largest);

#endif /* KERNEL_MEM_H */
