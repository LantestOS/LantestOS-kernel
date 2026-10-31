/*
 * Lantest OS - page cache with LRU and dirty writeback (M4-21)
 */
#pragma once

#include <types.h>
#include <fs.h>

u64 page_cache_get(struct file *file, u64 page_index, int *hit);
u64 page_cache_phys(struct inode *inode, u64 page_index);
void page_cache_mark_dirty(struct inode *inode, u64 page_index);
void page_cache_flush(struct inode *inode, u64 start_page, u64 end_page);
