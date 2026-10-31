/*
 * Lantest OS - virtio-blk driver (legacy PCI interface)
 */
#pragma once

#include <types.h>

int virtio_blk_init(void);
int virtio_blk_selftest(void);
int virtio_blk_present(void);

/* Sector size is fixed at 512 bytes; count is the number of sectors. */
int virtio_blk_read(u64 sector, void *buf, u32 count);
int virtio_blk_write(u64 sector, const void *buf, u32 count);

u64 virtio_blk_capacity(void);
