/*
 * Lantest OS - ELF64 user program loader (M3c-2)
 *
 * The kernel embeds the user ELF files as raw blobs; this loader parses the
 * ELF headers and maps PT_LOAD segments into a task PML4. All user mappings
 * live in PML4[1] (USER_BASE), which is also what paging_destroy_user_space()
 * walks when the task is reaped.
 */
#include <types.h>
#include <kprintf.h>
#include <pmm.h>
#include <elf.h>
#include <x86_64/paging.h>
#include <x86_64/usermode.h>
#include <task.h>
#include <vma.h>

#define ELF_MAX_PHNUM 32u
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(u64)(PAGE_SIZE - 1))

static int elf_validate(const Elf64_Ehdr *eh, u64 size)
{
    if (size < sizeof(Elf64_Ehdr))
        return -1;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F')
        return -1;
    if (eh->ident[4] != 2 || eh->ident[5] != 1)   /* ELFCLASS64, ELFDATA2LSB */
        return -1;
    if (eh->machine != ELF_EM_X86_64)
        return -1;
    if (eh->type != ELF_ET_EXEC && eh->type != ELF_ET_DYN)
        return -1;
    if (eh->phentsize != sizeof(Elf64_Phdr))
        return -1;
    if (eh->phnum == 0 || eh->phnum > ELF_MAX_PHNUM)
        return -1;
    if (eh->phoff > size)
        return -1;
    if ((u64)eh->phnum * eh->phentsize > size - eh->phoff)
        return -1;
    if (eh->entry < USER_BASE || eh->entry >= USER_STACK_TOP)
        return -1;
    return 0;
}

static int elf_validate_segment(const Elf64_Phdr *ph, u64 size)
{
    u64 end;

    if (ph->filesz > ph->memsz)
        return -1;
    /*
     * Linkers may emit an empty RW PT_LOAD (vaddr=0, memsz=0) when the
     * program has no .data/.bss. It loads nothing, so ignore it before any
     * address/range validation.
     */
    if (ph->memsz == 0)
        return 0;
    if (ph->offset > size || ph->filesz > size - ph->offset)
        return -1;
    if (ph->vaddr < USER_BASE)
        return -1;
    end = ph->vaddr + ph->memsz;
    if (end < ph->vaddr)
        return -1;
    /* Leave the user stack page at the very top of the user range free. */
    if (end > USER_STACK_TOP - PAGE_SIZE)
        return -1;
    return 0;
}

static void zero_range_in_page(u8 *page, u64 page_vaddr, u64 start, u64 end)
{
    for (u64 a = start; a < end; ++a)
        page[a - page_vaddr] = 0;
}

static void copy_range_in_page(u8 *page, u64 page_vaddr,
                               const u8 *src, u64 start, u64 end)
{
    for (u64 a = start; a < end; ++a)
        page[a - page_vaddr] = *src++;
}

int elf_load_user(u64 pml4_phys, const u8 *data, u64 size, u64 *entry_out)
{
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *ph;
    u64 page;

    if (data == NULL || entry_out == NULL)
        return -1;

    eh = (const Elf64_Ehdr *)data;
    if (elf_validate(eh, size) != 0)
        return -1;

    ph = (const Elf64_Phdr *)(data + eh->phoff);

    kprintf("[elf] entry=%p phnum=%u phoff=%lu\n",
            (void *)eh->entry, (unsigned)eh->phnum, eh->phoff);

    /* Pass 1: validate all PT_LOAD segments and map their pages. */
    for (u32 i = 0; i < eh->phnum; ++i) {
        const Elf64_Phdr *p = &ph[i];
        u64 start;
        u64 end;

        if (p->type != ELF_PT_LOAD)
            continue;
        if (elf_validate_segment(p, size) != 0)
            return -1;
        if (p->memsz == 0)
            continue;   /* empty segment: nothing to map or copy */

        kprintf("[elf] PT_LOAD vaddr=%p off=%lu filesz=%lu memsz=%lu\n",
                (void *)p->vaddr, p->offset, p->filesz, p->memsz);

        start = PAGE_ALIGN_DOWN(p->vaddr);
        end = PAGE_ALIGN_UP(p->vaddr + p->memsz);
        for (page = start; page < end; page += PAGE_SIZE) {
            if (paging_ensure_user_page(pml4_phys, page) == 0)
                return -1;
        }
    }

    /* Pass 2: zero every .bss range (p_memsz > p_filesz). */
    for (u32 i = 0; i < eh->phnum; ++i) {
        const Elf64_Phdr *p = &ph[i];
        u64 zstart;
        u64 zend;

        if (p->type != ELF_PT_LOAD || p->memsz <= p->filesz)
            continue;

        zstart = p->vaddr + p->filesz;
        zend = p->vaddr + p->memsz;
        for (page = PAGE_ALIGN_DOWN(zstart); page < zend; page += PAGE_SIZE) {
            u64 phys = paging_ensure_user_page(pml4_phys, page);
            u64 a = page > zstart ? page : zstart;
            u64 b = (page + PAGE_SIZE) < zend ? (page + PAGE_SIZE) : zend;

            if (phys == 0)
                return -1;
            if (a < b)
                zero_range_in_page((u8 *)(usize)phys, page, a, b);
        }
    }

    /* Pass 3: copy file-backed ranges. Runs last so file data wins in any
     * page shared by multiple segments. */
    for (u32 i = 0; i < eh->phnum; ++i) {
        const Elf64_Phdr *p = &ph[i];
        u64 fstart;
        u64 fend;

        if (p->type != ELF_PT_LOAD || p->filesz == 0)
            continue;

        fstart = p->vaddr;
        fend = p->vaddr + p->filesz;
        for (page = PAGE_ALIGN_DOWN(fstart); page < fend; page += PAGE_SIZE) {
            u64 phys = paging_ensure_user_page(pml4_phys, page);
            u64 a = page > fstart ? page : fstart;
            u64 b = (page + PAGE_SIZE) < fend ? (page + PAGE_SIZE) : fend;
            const u8 *src = data + p->offset + (a - fstart);

            if (phys == 0)
                return -1;
            if (a < b)
                copy_range_in_page((u8 *)(usize)phys, page, src, a, b);
        }
    }

    *entry_out = eh->entry;
    return 0;
}

int elf_load_vmas(struct task *t, struct file *file,
                  const u8 *data, u64 size, u64 *entry_out)
{
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *ph;

    if (t == NULL || file == NULL || data == NULL || entry_out == NULL)
        return -1;

    eh = (const Elf64_Ehdr *)data;
    if (elf_validate(eh, size) != 0)
        return -1;

    ph = (const Elf64_Phdr *)(data + eh->phoff);

    for (u32 i = 0; i < eh->phnum; ++i) {
        const Elf64_Phdr *p = &ph[i];
        u64 start;
        u64 end;
        u64 foff;
        u64 fend;
        u32 flags = VM_READ | VM_FILE;

        if (p->type != ELF_PT_LOAD)
            continue;
        if (elf_validate_segment(p, size) != 0)
            return -1;
        if (p->memsz == 0)
            continue;

        start = PAGE_ALIGN_DOWN(p->vaddr);
        end = PAGE_ALIGN_UP(p->vaddr + p->memsz);
        foff = p->offset - (p->vaddr - start);
        fend = p->filesz > 0 ? (p->offset + p->filesz) : foff;

        if ((p->flags & 2u) != 0)
            flags |= VM_WRITE;
        if ((p->flags & 1u) != 0)
            flags |= VM_EXEC;

        if (vma_add_file(t, start, end, flags, file, foff, fend) != 0)
            return -1;

        kprintf("[elf-lazy] vaddr=%p off=%lu filesz=%lu memsz=%lu\n",
                (void *)p->vaddr, p->offset, p->filesz, p->memsz);
    }

    *entry_out = eh->entry;
    return 0;
}
