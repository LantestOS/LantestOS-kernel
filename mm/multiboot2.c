/*
 * Lantest OS - Multiboot2 tag walking
 */
#include <types.h>
#include <multiboot2.h>

#define MB2_MAX_INFO_SIZE (16ULL * 1024 * 1024)

u64 mb2_total_size(u64 info_addr)
{
    const struct mb2_info *info;

    if (info_addr == 0)
        return 0;

    info = (const struct mb2_info *)info_addr;
    if (info->total_size < 16 || info->total_size > MB2_MAX_INFO_SIZE)
        return 0;

    return info->total_size;
}

const struct mb2_tag_mmap *mb2_find_mmap(u64 info_addr)
{
    const struct mb2_info *info;
    const u8 *p;
    const u8 *end;

    if (mb2_total_size(info_addr) == 0)
        return NULL;

    info = (const struct mb2_info *)info_addr;
    p = (const u8 *)info_addr + 8;
    end = (const u8 *)info_addr + info->total_size;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)p;

        if (tag->type == MB2_TAG_END)
            break;
        if (tag->size < sizeof(struct mb2_tag))
            break;
        if ((u64)(p + tag->size) > (u64)end)
            break;

        if (tag->type == MB2_TAG_MMAP)
            return (const struct mb2_tag_mmap *)tag;

        p += (tag->size + 7u) & ~7u;   /* tags are 8-byte aligned */
    }

    return NULL;
}
