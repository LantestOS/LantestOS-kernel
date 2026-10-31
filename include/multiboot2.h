/*
 * Lantest OS - Multiboot2 information structures (public specification).
 *
 * Only the tags the kernel currently consumes are described: the memory map
 * and the end tag. Tag walking is done in mm/multiboot2.c.
 */
#pragma once

#include <types.h>

#define MB2_BOOTLOADER_MAGIC  0x36D76289u
#define MB2_TAG_END           0u
#define MB2_TAG_MMAP          6u
#define MB2_MEMORY_AVAILABLE  1u

struct mb2_info {
    u32 total_size;
    u32 reserved;
};

struct mb2_tag {
    u32 type;
    u32 size;
};

struct mb2_mmap_entry {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
};

struct mb2_tag_mmap {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
    /* struct mb2_mmap_entry entries[] follows, entry_size bytes apart */
};

/* Find the memory map tag; returns NULL if absent or malformed. */
const struct mb2_tag_mmap *mb2_find_mmap(u64 info_addr);

/* Total size of the Multiboot2 information structure, 0 if invalid. */
u64 mb2_total_size(u64 info_addr);
