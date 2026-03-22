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
 * 64 KB kernel heap.
 *
 * Sized to cover dynamic thread stacks (up to 16 × DEFAULT_STACK_SIZE = 32 KB
 * at peak) plus kernel object allocations.  The former static stack_pool
 * (64 KB BSS) has been removed, so this growth is a net SRAM win.
 */
#define HEAP_SIZE (64u * 1024u)   /* 64 KB — absorbs dynamic thread stacks */

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
