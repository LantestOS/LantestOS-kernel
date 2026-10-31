/*
 * Lantest OS - kernel heap allocator (kmalloc/kfree)
 */
#pragma once

#include <types.h>

/* Initialize the heap (allocates the first region from the PMM). */
void kmalloc_init(void);

/* Allocate/free kernel heap memory. kmalloc(0) returns NULL. */
void *kmalloc(usize size);
void kfree(void *ptr);

/* Allocate/write/free a mix of sizes and report PASS/FAIL. */
void kmalloc_self_test(void);

/*
 * Heap statistics (used for logging and tests):
 *   region_count  - number of 4 KiB regions currently owned by the heap
 *   used_bytes    - bytes in allocated blocks (including block headers)
 *   free_bytes    - bytes in free blocks (including block headers)
 *   largest_free  - size of the largest single free block
 */
void kmalloc_stats(u64 *region_count, u64 *used_bytes,
                   u64 *free_bytes, u64 *largest_free);
