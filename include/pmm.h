/*
 * Lantest OS - physical page frame allocator (4 KiB pages, bitmap)
 */
#pragma once

#include <types.h>

#define PAGE_SIZE 4096u

/*
 * Initialize the allocator from the Multiboot2 memory map.
 * kernel_start/kernel_end delimit the kernel image that must stay reserved.
 * The allocator currently manages the first 1 GiB only (the identity map).
 */
void pmm_init(u64 mb2_info, u64 kernel_start, u64 kernel_end);

/* Allocate/free one 4 KiB page; alloc returns 0 on failure. */
u64  pmm_alloc_page(void);
void pmm_free_page(u64 addr);

/*
 * Allocate/free a physically contiguous run of pages (needed for kernel
 * stacks and heap regions until per-task paging exists).
 */
u64  pmm_alloc_pages(u32 count);
void pmm_free_pages(u64 addr, u32 count);

void pmm_ref_inc(u64 addr);
u64  pmm_ref_count(u64 addr);

u64  pmm_free_count(void);
u64  pmm_total_count(void);

/* Allocate/write/verify/free a few pages and report PASS/FAIL. */
void pmm_self_test(void);
