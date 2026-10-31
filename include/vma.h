/*
 * Lantest OS - virtual memory area (VMA) registry
 */
#pragma once

#include <types.h>
#include <fs.h>

#define VM_READ   0x1u
#define VM_WRITE  0x2u
#define VM_ANON   0x4u
#define VM_FILE   0x8u
#define VM_EXEC   0x10u
#define VM_SHARED 0x20u

struct task;

struct vm_area {
    u64 start;
    u64 end;
    u32 flags;
    struct file *file;       /* backing file, NULL for anonymous */
    u64 file_offset;         /* file offset corresponding to start */
    u64 file_data_end;       /* absolute file offset of valid data end */
    struct vm_area *next;
};

void vma_free_all(struct task *t);
int vma_add(struct task *t, u64 start, u64 end, u32 flags);
int vma_add_file(struct task *t, u64 start, u64 end, u32 flags,
                 struct file *file, u64 file_offset, u64 file_data_end);
struct vm_area *vma_find(struct task *t, u64 addr);
int vma_copy(struct task *dst, struct task *src);
int vma_unmap_range(struct task *t, u64 start, u64 end);
int vma_protect_range(struct task *t, u64 start, u64 end, u32 prot);
