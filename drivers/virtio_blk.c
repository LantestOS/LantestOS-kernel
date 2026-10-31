/*
 * Lantest OS - virtio-blk driver (legacy PCI, polling, no interrupts)
 *
 * Flow:
 *   1. PCI scan for vendor 0x1AF4 / device 0x1001 (legacy virtio-blk)
 *   2. enable I/O + bus master, legacy virtio status handshake
 *   3. one 4 KiB-aligned virtqueue: descriptors + available ring + used ring
 *   4. each request is a 3-descriptor chain: header -> data -> status
 *   5. notify the device and poll the used ring
 *
 * The kernel identity maps low memory, so virtual addresses are valid
 * physical addresses for the device's DMA.
 */
#include <types.h>
#include <io.h>
#include <spinlock.h>
#include <pci.h>
#include <pmm.h>
#include <kprintf.h>
#include <virtio_blk.h>

#define VIRTIO_VENDOR 0x1AF4u
#define VIRTIO_BLK_LEGACY 0x1001u
#define VIRTIO_BLK_MODERN 0x1042u

#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_ADDR     0x08
#define VIRTIO_PCI_QUEUE_SIZE     0x0C
#define VIRTIO_PCI_QUEUE_SELECT   0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_CONFIG         0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08

#define VIRTIO_BLK_T_IN  0u
#define VIRTIO_BLK_T_OUT 1u

#define VRING_DESC_F_NEXT  0x1u
#define VRING_DESC_F_WRITE 0x2u

#define SECTOR_SIZE 512u
#define QUEUE_PAGES 3u
#define MAX_IO_BYTES 65536u

struct vq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct vq_used_elem {
    u32 id;
    u32 len;
} __attribute__((packed));

struct virtio_blk_req {
    u32 type;
    u32 reserved;
    u64 sector;
} __attribute__((packed));

static int blk_present;
static u16 io_base;
static u32 qsize;
static u8 *vq_mem;
static u64 vq_phys;
static struct vq_desc *vq_desc;
static u16 *vq_avail_idx;
static u16 *vq_avail_ring;
static volatile u16 *vq_used_idx;
static struct vq_used_elem *vq_used_ring;
static u16 last_used;
static u64 capacity_sectors;

static struct virtio_blk_req blk_req __attribute__((aligned(16)));
static u8 blk_status;
static spinlock_t blk_io_lock = SPINLOCK_INIT;
static u8 sector_buf[SECTOR_SIZE] __attribute__((aligned(16)));

static const u8 boot_magic[8] = { 'L', 'N', 'T', 'S', 'T', 'B', 'L', 'K' };

static void zero_bytes(u8 *p, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        p[i] = 0;
}

static int mem_equal(const u8 *a, const u8 *b, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static int virtio_blk_rw(u64 sector, void *buf, u32 count, int write)
{
    u64 bytes;
    u16 slot;
    u32 spin;
    u64 irqflags;
    int rc = -1;

    if (!blk_present || buf == NULL || count == 0)
        return -1;
    if (count > MAX_IO_BYTES / SECTOR_SIZE)
        return -1;

    bytes = (u64)count * SECTOR_SIZE;
    if (sector + count < sector)
        return -1;

    spin_lock_irqsave(&blk_io_lock, &irqflags);

    blk_req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    blk_req.reserved = 0;
    blk_req.sector = sector;
    blk_status = 0xFF;

    vq_desc[0].addr = (u64)(usize)&blk_req;
    vq_desc[0].len = sizeof(blk_req);
    vq_desc[0].flags = VRING_DESC_F_NEXT;
    vq_desc[0].next = 1;

    vq_desc[1].addr = (u64)(usize)buf;
    vq_desc[1].len = (u32)bytes;
    vq_desc[1].flags = VRING_DESC_F_NEXT |
                       (write ? 0u : VRING_DESC_F_WRITE);
    vq_desc[1].next = 2;

    vq_desc[2].addr = (u64)(usize)&blk_status;
    vq_desc[2].len = 1;
    vq_desc[2].flags = VRING_DESC_F_WRITE;
    vq_desc[2].next = 0;

    slot = (u16)(*vq_avail_idx % qsize);
    vq_avail_ring[slot] = 0;
    __sync_synchronize();
    (*vq_avail_idx)++;
    __sync_synchronize();

    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    for (spin = 0; spin < 50000000u; ++spin) {
        if (*vq_used_idx != last_used)
            break;
        __asm__ volatile ("pause");
    }

    if (*vq_used_idx == last_used) {
        kprintf("[virtio] I/O timeout: sector=%lu count=%u\n", sector, count);
        goto out;
    }

    {
        u16 ui = (u16)(last_used % qsize);
        u32 id = vq_used_ring[ui].id;

        last_used++;
        if (id != 0 || blk_status != 0) {
            kprintf("[virtio] I/O error: id=%u status=%x\n", id, blk_status);
            goto out;
        }
    }

    __sync_synchronize();
    rc = 0;

out:
    spin_unlock_irqrestore(&blk_io_lock, irqflags);
    return rc;
}

int virtio_blk_read(u64 sector, void *buf, u32 count)
{
    return virtio_blk_rw(sector, buf, count, 0);
}

int virtio_blk_write(u64 sector, const void *buf, u32 count)
{
    return virtio_blk_rw(sector, (void *)buf, count, 1);
}

int virtio_blk_present(void)
{
    return blk_present;
}

u64 virtio_blk_capacity(void)
{
    return capacity_sectors;
}

int virtio_blk_init(void)
{
    u8 bus;
    u8 dev;
    u8 func;
    u32 bar0;
    u32 command;
    u32 used_off;
    u32 avail_off;
    u8 status;

    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_LEGACY,
                         &bus, &dev, &func)) {
        if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_MODERN,
                            &bus, &dev, &func)) {
            kprintf("[virtio] found modern-only virtio-blk; legacy "
                    "interface unsupported\n");
        } else {
            kprintf("[virtio] no virtio-blk PCI device\n");
        }
        return 0;
    }

    bar0 = pci_read32(bus, dev, func, 0x10);
    if ((bar0 & 1u) == 0) {
        kprintf("[virtio] BAR0 is not an I/O BAR (%x)\n", bar0);
        return 0;
    }
    io_base = (u16)(bar0 & ~3u);

    command = pci_read32(bus, dev, func, 0x04);
    command |= 0x5u;   /* I/O space enable + bus master (DMA) */
    pci_write32(bus, dev, func, 0x04, command);

    /* Reset and acknowledge the device. */
    outb(io_base + VIRTIO_PCI_STATUS, 0);
    for (u32 i = 0; i < 1000; ++i) {
        if (inb(io_base + VIRTIO_PCI_STATUS) == 0)
            break;
    }
    status = VIRTIO_STATUS_ACKNOWLEDGE;
    outb(io_base + VIRTIO_PCI_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    outb(io_base + VIRTIO_PCI_STATUS, status);

    /* We do not need any optional feature bits. */
    (void)inl(io_base + VIRTIO_PCI_HOST_FEATURES);
    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, 0);

    status |= VIRTIO_STATUS_FEATURES_OK;
    outb(io_base + VIRTIO_PCI_STATUS, status);
    if ((inb(io_base + VIRTIO_PCI_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        kprintf("[virtio] device rejected FEATURES_OK\n");
        return 0;
    }

    outw(io_base + VIRTIO_PCI_QUEUE_SELECT, 0);
    qsize = inw(io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsize < 3 || qsize > 256) {
        kprintf("[virtio] unsupported queue size %u\n", qsize);
        return 0;
    }

    vq_mem = (u8 *)pmm_alloc_pages(QUEUE_PAGES);
    if (vq_mem == NULL) {
        kprintf("[virtio] out of memory for virtqueue\n");
        return 0;
    }
    vq_phys = (u64)(usize)vq_mem;
    zero_bytes(vq_mem, QUEUE_PAGES * 4096u);

    avail_off = 16u * qsize;
    used_off = (avail_off + 4u + 2u * qsize + 4095u) & ~4095u;
    if (used_off + 4u + 8u * qsize > QUEUE_PAGES * 4096u) {
        kprintf("[virtio] virtqueue does not fit in %u pages\n", QUEUE_PAGES);
        return 0;
    }

    vq_desc = (struct vq_desc *)vq_mem;
    vq_avail_idx = (u16 *)(vq_mem + avail_off + 2);
    vq_avail_ring = (u16 *)(vq_mem + avail_off + 4);
    vq_used_idx = (volatile u16 *)(vq_mem + used_off + 2);
    vq_used_ring = (struct vq_used_elem *)(vq_mem + used_off + 4);
    last_used = 0;

    /* Legacy virtio-pci QueuePFN holds the physical page frame number. */
    outl(io_base + VIRTIO_PCI_QUEUE_ADDR, (u32)(vq_phys >> 12));

    status |= VIRTIO_STATUS_DRIVER_OK;
    outb(io_base + VIRTIO_PCI_STATUS, status);

    capacity_sectors = (u64)inl(io_base + VIRTIO_PCI_CONFIG) |
                       ((u64)inl(io_base + VIRTIO_PCI_CONFIG + 4) << 32);

    blk_present = 1;
    kprintf("[virtio] virtio-blk ready: I/O=%x qsize=%u capacity=%lu sectors\n",
            io_base, qsize, capacity_sectors);
    return 1;
}

int virtio_blk_selftest(void)
{
    u32 boot_count;
    u64 sector;

    if (!blk_present)
        return 0;

    sector = capacity_sectors > 1 ? capacity_sectors - 1 : 0;

    if (virtio_blk_read(sector, sector_buf, 1) != 0) {
        kprintf("[virtio] self-test: read failed\n");
        return 0;
    }

    if (mem_equal(sector_buf, boot_magic, sizeof(boot_magic))) {
        boot_count = (u32)sector_buf[8] |
                     ((u32)sector_buf[9] << 8) |
                     ((u32)sector_buf[10] << 16) |
                     ((u32)sector_buf[11] << 24);
        boot_count++;
    } else {
        boot_count = 1;
        zero_bytes(sector_buf, SECTOR_SIZE);
        for (u32 i = 0; i < sizeof(boot_magic); ++i)
            sector_buf[i] = boot_magic[i];
    }

    sector_buf[8] = (u8)(boot_count & 0xFF);
    sector_buf[9] = (u8)((boot_count >> 8) & 0xFF);
    sector_buf[10] = (u8)((boot_count >> 16) & 0xFF);
    sector_buf[11] = (u8)((boot_count >> 24) & 0xFF);

    if (virtio_blk_write(sector, sector_buf, 1) != 0) {
        kprintf("[virtio] self-test: write failed\n");
        return 0;
    }

    zero_bytes(sector_buf, SECTOR_SIZE);
    if (virtio_blk_read(sector, sector_buf, 1) != 0) {
        kprintf("[virtio] self-test: verify read failed\n");
        return 0;
    }

    if (!mem_equal(sector_buf, boot_magic, sizeof(boot_magic)) ||
        ((u32)sector_buf[8] |
         ((u32)sector_buf[9] << 8) |
         ((u32)sector_buf[10] << 16) |
         ((u32)sector_buf[11] << 24)) != boot_count) {
        kprintf("[virtio] self-test: verify mismatch\n");
        return 0;
    }

    kprintf("[virtio] self-test: PASS (disk boot_count=%lu)\n",
            (u64)boot_count);
    return 1;
}
