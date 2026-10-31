/*
 * Lantest OS - x86_64 paging helpers
 *
 * The kernel stays identity mapped below 1 GiB in PML4[0] (U/S = 0).
 * Ring 3 mappings are placed in PML4[1] (0x0000_0080_0000_0000 = 512 GiB),
 * with the U/S bit set at every level so that the CPU allows user access.
 */
#include <types.h>
#include <pmm.h>
#include <kprintf.h>
#include <task.h>
#include <vma.h>
#include <vfs.h>
#include <page_cache.h>
#include <x86_64/paging.h>
#include <x86_64/usermode.h>

#define PML4_ENTRIES 512u
#define PAGE_MASK    0xFFFFFFFFFFFFF000ULL

#define PTE_PRESENT  0x1u
#define PTE_WRITE    0x2u
#define PTE_USER     0x4u
#define PTE_COW      0x200u   /* software: copy-on-write */
#define PTE_SHARED   0x400u   /* software: MAP_SHARED page */

u64 paging_read_cr3(void)
{
    u64 value;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

void paging_write_cr3(u64 cr3)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void *paging_current_pml4(void)
{
    return (void *)(usize)paging_read_cr3();
}

static void zero_page(u64 phys)
{
    u8 *p = (u8 *)(usize)phys;

    for (u32 i = 0; i < PAGE_SIZE; ++i)
        p[i] = 0;
}

static u64 *page_table(u64 entry_value)
{
    return (u64 *)(usize)(entry_value & PAGE_MASK);
}

static u64 *next_level(u64 *table, u32 index)
{
    if ((table[index] & PTE_PRESENT) == 0) {
        u64 page = pmm_alloc_page();

        if (page == 0)
            return NULL;
        zero_page(page);
        table[index] = page | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        table[index] |= PTE_USER;
    }

    return page_table(table[index]);
}

static u64 *walk_level(u64 *table, u32 index)
{
    if ((table[index] & PTE_PRESENT) == 0)
        return NULL;
    return page_table(table[index]);
}

u64 paging_create_kernel_pml4(void)
{
    u64 new_phys = pmm_alloc_page();
    u64 *dst;
    const u64 *src;

    if (new_phys == 0)
        return 0;

    dst = (u64 *)(usize)new_phys;
    src = (const u64 *)(usize)paging_read_cr3();

    for (u32 i = 0; i < PML4_ENTRIES; ++i)
        dst[i] = src[i];

    return new_phys;
}

void paging_clear_user_root(u64 pml4_phys)
{
    u64 *pml4 = (u64 *)(usize)pml4_phys;

    if (pml4 != NULL)
        pml4[1] = 0;
}

int paging_map_user(u64 pml4_phys, u64 virt, u64 phys, u64 flags)
{
    u32 i4 = (u32)((virt >> 39) & 0x1FF);
    u32 i3 = (u32)((virt >> 30) & 0x1FF);
    u32 i2 = (u32)((virt >> 21) & 0x1FF);
    u32 i1 = (u32)((virt >> 12) & 0x1FF);

    u64 *pml4 = (u64 *)(usize)pml4_phys;
    u64 *pdpt = next_level(pml4, i4);
    if (pdpt == NULL)
        return -1;

    u64 *pd = next_level(pdpt, i3);
    if (pd == NULL)
        return -1;

    u64 *pt = next_level(pd, i2);
    if (pt == NULL)
        return -1;

    pt[i1] = (phys & PAGE_MASK) | flags | PTE_PRESENT | PTE_USER;
    return 0;
}

u64 paging_ensure_user_page(u64 pml4_phys, u64 virt)
{
    u32 i4 = (u32)((virt >> 39) & 0x1FF);
    u32 i3 = (u32)((virt >> 30) & 0x1FF);
    u32 i2 = (u32)((virt >> 21) & 0x1FF);
    u32 i1 = (u32)((virt >> 12) & 0x1FF);

    u64 *pml4 = (u64 *)(usize)pml4_phys;
    u64 *pdpt = next_level(pml4, i4);
    if (pdpt == NULL)
        return 0;

    u64 *pd = next_level(pdpt, i3);
    if (pd == NULL)
        return 0;

    u64 *pt = next_level(pd, i2);
    if (pt == NULL)
        return 0;

    if ((pt[i1] & PTE_PRESENT) == 0) {
        u64 phys = pmm_alloc_page();

        if (phys == 0)
            return 0;
        zero_page(phys);
        pt[i1] = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        pt[i1] |= PTE_USER;
    }

    return pt[i1] & PAGE_MASK;
}

u64 paging_get_user_page(u64 pml4_phys, u64 virt)
{
    u32 i4 = (u32)((virt >> 39) & 0x1FF);
    u32 i3 = (u32)((virt >> 30) & 0x1FF);
    u32 i2 = (u32)((virt >> 21) & 0x1FF);
    u32 i1 = (u32)((virt >> 12) & 0x1FF);

    u64 *pml4 = (u64 *)(usize)pml4_phys;
    u64 *pdpt = walk_level(pml4, i4);
    if (pdpt == NULL)
        return 0;

    u64 *pd = walk_level(pdpt, i3);
    if (pd == NULL)
        return 0;

    u64 *pt = walk_level(pd, i2);
    if (pt == NULL)
        return 0;

    if ((pt[i1] & PTE_PRESENT) == 0)
        return 0;

    return pt[i1] & PAGE_MASK;
}

u64 *paging_user_pte(u64 pml4_phys, u64 virt)
{
    u32 i4 = (u32)((virt >> 39) & 0x1FF);
    u32 i3 = (u32)((virt >> 30) & 0x1FF);
    u32 i2 = (u32)((virt >> 21) & 0x1FF);
    u32 i1 = (u32)((virt >> 12) & 0x1FF);
    u64 *pml4 = (u64 *)(usize)pml4_phys;
    u64 *pdpt = walk_level(pml4, i4);
    u64 *pd;
    u64 *pt;

    if (pdpt == NULL)
        return NULL;
    pd = walk_level(pdpt, i3);
    if (pd == NULL)
        return NULL;
    pt = walk_level(pd, i2);
    if (pt == NULL)
        return NULL;
    return &pt[i1];
}

int paging_copy_user_space(u64 dst_pml4, u64 src_pml4)
{
    int ok = 0;

    for (u64 virt = USER_BASE; virt < USER_STACK_TOP; virt += PAGE_SIZE) {
        u64 *src_pte = paging_user_pte(src_pml4, virt);
        u64 flags;
        u64 phys;
        u64 child_flags;

        if (src_pte == NULL || (*src_pte & PTE_PRESENT) == 0)
            continue;

        flags = *src_pte;
        phys = flags & PAGE_MASK;
        child_flags = PTE_USER;

        if ((flags & PTE_SHARED) != 0) {
            /* MAP_SHARED: keep the same writable physical page. */
            pmm_ref_inc(phys);
            if (paging_map_user(dst_pml4, virt, phys,
                                PTE_WRITE | PTE_SHARED) != 0) {
                pmm_free_page(phys);
                ok = -1;
                break;
            }
            continue;
        }

        if ((flags & PTE_WRITE) != 0) {
            /* Write-protect both parent and child; first writer copies. */
            *src_pte = (flags & ~PTE_WRITE) | PTE_COW;
            child_flags |= PTE_COW;
        } else if ((flags & PTE_COW) != 0) {
            child_flags |= PTE_COW;
        }

        pmm_ref_inc(phys);

        if (paging_map_user(dst_pml4, virt, phys, child_flags) != 0) {
            pmm_free_page(phys);
            ok = -1;
            break;
        }
    }

    /* Parent's writable PTEs were just made read-only; flush stale TLB. */
    paging_write_cr3(paging_read_cr3());
    return ok;
}

static u32 cow_logs;

int paging_handle_cow(u64 vaddr)
{
    u64 *pte = paging_user_pte(paging_read_cr3(), vaddr);
    u64 flags;
    u64 old_phys;
    u64 new_phys;
    u64 refs;
    u8 *src;
    u8 *dst;

    if (pte == NULL)
        return 0;
    flags = *pte;
    if ((flags & PTE_PRESENT) == 0 || (flags & PTE_COW) == 0)
        return 0;

    old_phys = flags & PAGE_MASK;
    refs = pmm_ref_count(old_phys);

    if (cow_logs < 8) {
        kprintf("[cow] vaddr=%p phys=%p refs=%lu\n",
                (void *)vaddr, (void *)old_phys, refs);
        cow_logs++;
    }

    if (refs > 1) {
        new_phys = pmm_alloc_page();
        if (new_phys == 0)
            return 0;

        src = (u8 *)(usize)old_phys;
        dst = (u8 *)(usize)new_phys;
        for (u32 i = 0; i < PAGE_SIZE; ++i)
            dst[i] = src[i];

        flags = (flags & ~PAGE_MASK) | new_phys;
        flags |= PTE_WRITE;
        flags &= ~PTE_COW;
        *pte = flags;

        pmm_free_page(old_phys);   /* drop this task's reference */
    } else {
        *pte = (flags | PTE_WRITE) & ~PTE_COW;
    }

    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

static u32 demand_logs;

int paging_handle_demand(u64 vaddr)
{
    struct task *t = task_current();
    struct vm_area *v;
    u64 *pte;
    u64 phys;

    if (t == NULL)
        return 0;

    v = vma_find(t, vaddr);
    if (v == NULL)
        return 0;

    pte = paging_user_pte(paging_read_cr3(), vaddr);
    if (pte != NULL && (*pte & PTE_PRESENT) != 0)
        return 0;   /* present but not COW: real protection fault */

    if ((v->flags & VM_SHARED) != 0 && (v->flags & VM_FILE) != 0 &&
        v->file != NULL && v->file->f_inode != NULL) {
        u64 page = vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 foff = v->file_offset + (page - v->start);
        u64 avail = (v->file_data_end > foff) ? (v->file_data_end - foff) : 0;
        u64 n = avail > PAGE_SIZE ? PAGE_SIZE : avail;
        int hit = 0;

        phys = page_cache_get(v->file, foff / PAGE_SIZE, &hit);
        if (phys != 0) {
            if (!hit && n > 0)
                (void)vfs_read_at(v->file, foff, (void *)(usize)phys, n);
            page_cache_mark_dirty(v->file->f_inode, foff / PAGE_SIZE);
            pmm_ref_inc(phys);
            if (paging_map_user(t->cr3, page, phys,
                                PTE_WRITE | PTE_SHARED) == 0) {
                if (demand_logs < 8) {
                    kprintf("[demand] shared vaddr=%p phys=%p pcache-hit=%d\n",
                            (void *)vaddr, (void *)phys, hit);
                    demand_logs++;
                }
                __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
                return 1;
            }
            pmm_free_page(phys);
        }
    }

    if ((v->flags & VM_FILE) != 0 && v->file != NULL &&
        (v->flags & VM_WRITE) == 0 && v->file->f_inode != NULL) {
        u64 page = vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 foff = v->file_offset + (page - v->start);
        u64 avail = (v->file_data_end > foff) ? (v->file_data_end - foff) : 0;
        u64 n = avail > PAGE_SIZE ? PAGE_SIZE : avail;

        if (n > 0 && (foff & (PAGE_SIZE - 1)) == 0) {
            int hit = 0;

            phys = page_cache_get(v->file, foff / PAGE_SIZE, &hit);
            if (phys != 0) {
                if (!hit) {
                    i64 got = vfs_read_at(v->file, foff,
                                          (void *)(usize)phys, n);
                    (void)got;
                }
                pmm_ref_inc(phys);
                if (paging_map_user(t->cr3, page, phys, 0) == 0) {
                    if (demand_logs < 8) {
                        kprintf("[demand] vaddr=%p phys=%p pcache-hit=%d flags=%x\n",
                                (void *)vaddr, (void *)phys, hit, v->flags);
                        demand_logs++;
                    }
                    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
                    return 1;
                }
                pmm_free_page(phys);
            }
        }
    }

    phys = paging_ensure_user_page(t->cr3, vaddr);
    if (phys == 0)
        return 0;

    if ((v->flags & VM_FILE) != 0 && v->file != NULL) {
        u64 page = vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 foff = v->file_offset + (page - v->start);
        u64 avail = (v->file_data_end > foff) ? (v->file_data_end - foff) : 0;
        u64 n = avail > PAGE_SIZE ? PAGE_SIZE : avail;

        if (n > 0) {
            i64 got = vfs_read_at(v->file, foff, (void *)(usize)phys, n);
            (void)got;
        }
    }

    pte = paging_user_pte(t->cr3, vaddr & ~(u64)(PAGE_SIZE - 1));
    if (pte != NULL && (v->flags & VM_WRITE) == 0)
        *pte = *pte & ~(u64)PTE_WRITE;

    if (demand_logs < 8) {
        kprintf("[demand] vaddr=%p phys=%p flags=%x\n",
                (void *)vaddr, (void *)phys, v->flags);
        demand_logs++;
    }

    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

void paging_unmap_range(u64 pml4_phys, u64 start, u64 end)
{
    u64 page;

    for (page = start & ~(u64)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        u64 *pte = paging_user_pte(pml4_phys, page);

        if (pte != NULL && (*pte & PTE_PRESENT) != 0) {
            u64 phys = *pte & PAGE_MASK;

            *pte = 0;
            pmm_free_page(phys);
            __asm__ volatile ("invlpg (%0)" : : "r"(page) : "memory");
        }
    }
}

void paging_protect_range(u64 pml4_phys, u64 start, u64 end, int write)
{
    u64 page;

    for (page = start & ~(u64)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        u64 *pte = paging_user_pte(pml4_phys, page);
        u64 phys;
        u64 refs;

        if (pte == NULL || (*pte & PTE_PRESENT) == 0)
            continue;

        if (!write) {
            *pte = *pte & ~(u64)(PTE_WRITE | PTE_COW);
        } else {
            phys = *pte & PAGE_MASK;
            refs = pmm_ref_count(phys);
            if (refs > 1)
                *pte = (*pte | PTE_COW) & ~(u64)PTE_WRITE;
            else
                *pte = (*pte | PTE_WRITE) & ~(u64)PTE_COW;
        }
        __asm__ volatile ("invlpg (%0)" : : "r"(page) : "memory");
    }
}

void paging_destroy_user_mappings(u64 pml4_phys)
{
    u64 *pml4 = (u64 *)(usize)pml4_phys;

    if (pml4 == NULL)
        return;

    if ((pml4[1] & PTE_PRESENT) == 0)
        return;

    u64 *pdpt = page_table(pml4[1]);

    for (u32 i = 0; i < PML4_ENTRIES; ++i) {
        u64 *pd;
        if ((pdpt[i] & PTE_PRESENT) == 0)
            continue;

        pd = page_table(pdpt[i]);
        for (u32 j = 0; j < PML4_ENTRIES; ++j) {
            u64 *pt;
            if ((pd[j] & PTE_PRESENT) == 0)
                continue;

            pt = page_table(pd[j]);
            for (u32 k = 0; k < PML4_ENTRIES; ++k) {
                if ((pt[k] & PTE_PRESENT) != 0)
                    pmm_free_page(pt[k] & PAGE_MASK);
            }
            pmm_free_page((u64)(usize)pt);
        }
        pmm_free_page((u64)(usize)pd);
    }
    pmm_free_page((u64)(usize)pdpt);

    pml4[1] = 0;
}

void paging_destroy_user_space(u64 pml4_phys)
{
    if (pml4_phys == 0)
        return;

    paging_destroy_user_mappings(pml4_phys);
    pmm_free_page(pml4_phys);
}
