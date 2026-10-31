/*
 * Lantest OS - virtual memory area (VMA) registry
 *
 * Anonymous and file-backed regions. File VMAs hold a file reference; fork
 * copies the VMA and takes another reference.
 */
#include <types.h>
#include <kmalloc.h>
#include <task.h>
#include <vma.h>

void vma_free_all(struct task *t)
{
    struct vm_area *v;

    if (t == NULL)
        return;

    v = t->vmas;
    while (v != NULL) {
        struct vm_area *next = v->next;
        if (v->file != NULL)
            file_close(v->file);
        kfree(v);
        v = next;
    }
    t->vmas = NULL;
}

static int vma_insert(struct task *t, struct vm_area *v)
{
    struct vm_area **link = &t->vmas;
    struct vm_area *prev = NULL;

    while (*link != NULL && (*link)->start < v->start) {
        prev = *link;
        link = &(*link)->next;
    }

    /* Merge ELF PT_LOAD segments that share/abut a page. */
    if (prev != NULL && v->file != NULL && prev->file == v->file &&
        v->start < prev->end) {
        if (v->end > prev->end)
            prev->end = v->end;
        prev->flags |= v->flags;
        {
            int v_has = v->file_data_end > v->file_offset;
            int p_has = prev->file_data_end > prev->file_offset;

            if (v_has && (!p_has || v->file_offset < prev->file_offset))
                prev->file_offset = v->file_offset;
        }
        if (v->file_data_end > prev->file_data_end)
            prev->file_data_end = v->file_data_end;
        file_close(v->file);
        kfree(v);
        return 0;
    }
    if (*link != NULL && v->file != NULL && (*link)->file == v->file &&
        (*link)->start < v->end) {
        struct vm_area *next = *link;

        next->start = v->start;
        if (v->end > next->end)
            next->end = v->end;
        next->flags |= v->flags;
        {
            int v_has = v->file_data_end > v->file_offset;
            int n_has = next->file_data_end > next->file_offset;

            if (v_has && (!n_has || v->file_offset < next->file_offset))
                next->file_offset = v->file_offset;
        }
        if (v->file_data_end > next->file_data_end)
            next->file_data_end = v->file_data_end;
        file_close(v->file);
        kfree(v);
        return 0;
    }

    v->next = *link;
    *link = v;
    return 0;
}

int vma_add(struct task *t, u64 start, u64 end, u32 flags)
{
    struct vm_area *v;

    if (t == NULL || start >= end)
        return -1;

    v = (struct vm_area *)kmalloc(sizeof(struct vm_area));
    if (v == NULL)
        return -1;
    v->start = start;
    v->end = end;
    v->flags = flags;
    v->file = NULL;
    v->file_offset = 0;
    v->file_data_end = 0;
    return vma_insert(t, v);
}

int vma_add_file(struct task *t, u64 start, u64 end, u32 flags,
                 struct file *file, u64 file_offset, u64 file_data_end)
{
    struct vm_area *v;

    if (t == NULL || start >= end || file == NULL)
        return -1;

    v = (struct vm_area *)kmalloc(sizeof(struct vm_area));
    if (v == NULL)
        return -1;
    v->start = start;
    v->end = end;
    v->flags = flags | VM_FILE;
    v->file = file;
    v->file_offset = file_offset;
    v->file_data_end = file_data_end;
    file_get(file);
    return vma_insert(t, v);
}

struct vm_area *vma_find(struct task *t, u64 addr)
{
    struct vm_area *v;

    if (t == NULL)
        return NULL;

    for (v = t->vmas; v != NULL; v = v->next) {
        if (addr >= v->start && addr < v->end)
            return v;
    }
    return NULL;
}

int vma_copy(struct task *dst, struct task *src)
{
    struct vm_area *v;

    if (dst == NULL || src == NULL)
        return -1;

    for (v = src->vmas; v != NULL; v = v->next) {
        if (v->file != NULL) {
            if (vma_add_file(dst, v->start, v->end, v->flags, v->file,
                             v->file_offset, v->file_data_end) != 0)
                return -1;
        } else if (vma_add(dst, v->start, v->end, v->flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static struct vm_area *vma_clone_part(struct vm_area *v, u64 start, u64 end)
{
    struct vm_area *n = (struct vm_area *)kmalloc(sizeof(struct vm_area));

    if (n == NULL)
        return NULL;
    n->start = start;
    n->end = end;
    n->flags = v->flags;
    n->file = v->file;
    n->file_offset = v->file_offset + (start - v->start);
    n->file_data_end = v->file_data_end;
    n->next = NULL;

    if (n->file != NULL)
        file_get(n->file);
    return n;
}

static int vma_split_at(struct task *t, u64 addr)
{
    struct vm_area *v;

    for (v = t->vmas; v != NULL; v = v->next) {
        if (addr > v->start && addr < v->end) {
            struct vm_area *right = vma_clone_part(v, addr, v->end);

            if (right == NULL)
                return -1;
            v->end = addr;
            right->next = v->next;
            v->next = right;
            return 0;
        }
    }
    return 0;
}

int vma_unmap_range(struct task *t, u64 start, u64 end)
{
    struct vm_area **link;

    if (t == NULL || start >= end)
        return -1;

    if (vma_split_at(t, start) != 0 || vma_split_at(t, end) != 0)
        return -1;

    link = &t->vmas;
    while (*link != NULL) {
        struct vm_area *v = *link;

        if (v->start >= start && v->end <= end) {
            *link = v->next;
            if (v->file != NULL)
                file_close(v->file);
            kfree(v);
        } else {
            link = &v->next;
        }
    }
    return 0;
}

int vma_protect_range(struct task *t, u64 start, u64 end, u32 prot)
{
    struct vm_area *v;

    if (t == NULL || start >= end)
        return -1;

    if (vma_split_at(t, start) != 0 || vma_split_at(t, end) != 0)
        return -1;

    for (v = t->vmas; v != NULL; v = v->next) {
        if (v->start >= start && v->end <= end)
            v->flags = (v->flags & (VM_FILE | VM_ANON)) | prot;
    }
    return 0;
}
