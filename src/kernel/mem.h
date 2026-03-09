#ifndef KERNEL_MEM_H
#define KERNEL_MEM_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Kernel heap size
 *
 * 64 KB is a reasonable fraction of the RP2040's 264 KB SRAM.  Adjust this
 * downward if thread stacks and static pools leave insufficient room.
 * ------------------------------------------------------------------------- */
/*
 * 32 KB kernel heap.
 *
 * RP2040 has 264 KB of SRAM shared with thread stacks (64 KB for 16 threads
 * at SERVICE_STACK_SIZE), the filesystem RAM buffer (16 KB), SDK buffers,
 * and code.  32 KB is a reasonable heap for Phase 1-3 workloads.
 * Increase to 64 KB if the thread count or stack sizes are reduced.
 */
#define HEAP_SIZE (32u * 1024u)   /* 32 KB kernel heap */

/* -------------------------------------------------------------------------
 * Memory manager API
 * ------------------------------------------------------------------------- */

/*
 * mem_init — initialise the kernel heap.
 *            Must be called once before any kmalloc/kfree calls.
 */
void mem_init(void);

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
 * mem_stats — fill in heap usage statistics.
 *   *used    — total bytes currently allocated (including headers)
 *   *free    — total free bytes (including headers of free blocks)
 *   *largest — size of the largest single contiguous free block (payload only)
 */
void mem_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *largest);

#endif /* KERNEL_MEM_H */
