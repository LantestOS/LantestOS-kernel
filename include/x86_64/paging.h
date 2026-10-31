/*
 * Lantest OS - x86_64 paging helpers
 */
#pragma once

#include <types.h>

u64  paging_read_cr3(void);
void paging_write_cr3(u64 cr3);

/* Current PML4 (identity mapped, so physical == virtual for now). */
void *paging_current_pml4(void);

/*
 * Allocate a new PML4 for a task and copy the current kernel mappings into
 * it. Returns the physical address of the new PML4 (0 on failure).
 * Note: this copies PML4[1] too; fork/exec must call paging_clear_user_root()
 * before building a brand-new user address space.
 */
u64 paging_create_kernel_pml4(void);

/* Set PML4[1] (the user half) to zero, detaching old user mappings. */
void paging_clear_user_root(u64 pml4_phys);

int paging_map_user(u64 pml4_phys, u64 virt, u64 phys, u64 flags);
u64 paging_ensure_user_page(u64 pml4_phys, u64 virt);

/* Walk the user page tables; returns the physical page or 0 if not mapped. */
u64 paging_get_user_page(u64 pml4_phys, u64 virt);

/*
 * Deep-copy every mapped user page from src PML4 into dst PML4 (new physical
 * pages). dst must have an empty PML4[1]. Returns 0 or -1.
 */
int paging_copy_user_space(u64 dst_pml4, u64 src_pml4);

/* Copy-on-write helpers. */
u64 *paging_user_pte(u64 pml4_phys, u64 virt);
int paging_handle_cow(u64 vaddr);
int paging_handle_demand(u64 vaddr);
void paging_unmap_range(u64 pml4_phys, u64 start, u64 end);
void paging_protect_range(u64 pml4_phys, u64 start, u64 end, int write);

void paging_destroy_user_mappings(u64 pml4_phys);
void paging_destroy_user_space(u64 pml4_phys);
