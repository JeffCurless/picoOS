#include "mem.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Boundary-tag heap allocator
 *
 * Design: first-fit, with immediate coalescing on free.
 *
 * Each allocation is preceded by a heap_block header.  Blocks form a simple
 * singly-linked list from the start of the heap to the end.  There is no
 * separate footer / boundary tag — coalescing walks forward from the freed
 * block to merge consecutive free blocks.
 *
 * Layout inside heap_memory:
 *
 *   [ heap_block | payload ... | heap_block | payload ... | ... ]
 *
 * heap_block.size is the size of the *payload* only (does not include the
 * header itself).
 *
 * Alignment: all allocations are rounded up to an 8-byte boundary so that
 * the payload is suitable for any fundamental C type.
 * ------------------------------------------------------------------------- */

#define ALIGN8(x)       (((x) + 7u) & ~7u)
#define SPLIT_THRESHOLD 32u   /* minimum leftover size to create a new block */
#define HEADER_SIZE     ALIGN8(sizeof(struct heap_block))

struct heap_block {
    uint32_t          size;   /* payload size in bytes (NOT including header) */
    bool              free;
    struct heap_block *next;  /* next block in the list (NULL = last block)   */
};

/* The heap lives in BSS so the linker accounts for it at link time. */
static uint8_t heap_memory[HEAP_SIZE] __attribute__((aligned(8)));

/* Head of the free-block list. */
static struct heap_block *heap_head = NULL;

/* -------------------------------------------------------------------------
 * mem_init
 * ------------------------------------------------------------------------- */
void mem_init(void)
{
    memset(heap_memory, 0, sizeof(heap_memory));

    heap_head       = (struct heap_block *)heap_memory;
    heap_head->size = HEAP_SIZE - (uint32_t)HEADER_SIZE;
    heap_head->free = true;
    heap_head->next = NULL;
}

/* -------------------------------------------------------------------------
 * kmalloc
 * ------------------------------------------------------------------------- */
void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* Round the requested size up to an 8-byte boundary. */
    uint32_t aligned_size = (uint32_t)ALIGN8(size);

    /* First-fit search. */
    struct heap_block *blk = heap_head;
    while (blk != NULL) {
        if (blk->free && blk->size >= aligned_size) {
            /* Found a suitable block.  Split it if the remainder is large
             * enough to form a useful block (header + SPLIT_THRESHOLD bytes). */
            uint32_t leftover = blk->size - aligned_size;

            if (leftover >= HEADER_SIZE + SPLIT_THRESHOLD) {
                /* Carve out a new free block from the remainder. */
                struct heap_block *new_blk =
                    (struct heap_block *)((uint8_t *)blk + HEADER_SIZE + aligned_size);
                new_blk->size = leftover - (uint32_t)HEADER_SIZE;
                new_blk->free = true;
                new_blk->next = blk->next;

                blk->size = aligned_size;
                blk->next = new_blk;
            }

            blk->free = false;
            return (uint8_t *)blk + HEADER_SIZE;
        }
        blk = blk->next;
    }

    /* No suitable block found. */
    return NULL;
}

/* -------------------------------------------------------------------------
 * kfree
 * ------------------------------------------------------------------------- */
void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    /* Recover the block header. */
    struct heap_block *blk =
        (struct heap_block *)((uint8_t *)ptr - HEADER_SIZE);

    blk->free = true;

    /* Coalesce: walk forward and merge all consecutive free blocks. */
    while (blk->next != NULL && blk->next->free) {
        struct heap_block *next = blk->next;
        /* Absorb next into blk. */
        blk->size += (uint32_t)HEADER_SIZE + next->size;
        blk->next  = next->next;
    }
}

/* -------------------------------------------------------------------------
 * mem_stats
 * ------------------------------------------------------------------------- */
void mem_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *largest)
{
    uint32_t total_used    = 0;
    uint32_t total_free    = 0;
    uint32_t largest_free  = 0;

    struct heap_block *blk = heap_head;
    while (blk != NULL) {
        uint32_t block_total = (uint32_t)HEADER_SIZE + blk->size;
        if (blk->free) {
            total_free += block_total;
            if (blk->size > largest_free) {
                largest_free = blk->size;
            }
        } else {
            total_used += block_total;
        }
        blk = blk->next;
    }

    if (used        != NULL) { *used        = total_used;   }
    if (free_bytes  != NULL) { *free_bytes  = total_free;   }
    if (largest     != NULL) { *largest     = largest_free; }
}
