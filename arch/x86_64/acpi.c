/*
 * Lantest OS - ACPI RSDP/MADT discovery (M4-32 SMP foundation)
 */
#include <types.h>
#include <acpi.h>
#include <multiboot2.h>
#include <kprintf.h>

#define ACPI_TAG_OLD_RSDP 14u
#define ACPI_TAG_NEW_RSDP 15u

struct acpi_sdt_header {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    char creator_id[4];
    u32 creator_revision;
} __attribute__((packed));

struct acpi_rsdp {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    u32 local_apic_address;
    u32 flags;
} __attribute__((packed));

static struct acpi_info info;

static int sig4(const char *p, const char *s)
{
    return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

static int checksum_ok(const u8 *p, u32 len)
{
    u8 sum = 0;
    for (u32 i = 0; i < len; ++i)
        sum = (u8)(sum + p[i]);
    return sum == 0;
}

static u32 cpu_apic_id(void)
{
    u32 a, b, c, d;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1));
    (void)a; (void)c; (void)d;
    return (b >> 24) & 0xffu;
}

static const u8 *scan_rsdp(void)
{
    for (u64 addr = 0xE0000; addr < 0x100000; addr += 16) {
        const u8 *p = (const u8 *)addr;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' &&
            p[3] == ' ' && p[4] == 'P' && p[5] == 'T' &&
            p[6] == 'R' && p[7] == ' ' &&
            checksum_ok(p, 20))
            return p;
    }
    return NULL;
}

static const u8 *find_rsdp(u64 mb2_info)
{
    const u8 *p;
    const u8 *end;

    if (mb2_total_size(mb2_info) == 0)
        return scan_rsdp();

    p = (const u8 *)mb2_info + 8;
    end = (const u8 *)mb2_info + mb2_total_size(mb2_info);
    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)p;

        if (tag->type == MB2_TAG_END || tag->size < sizeof(struct mb2_tag))
            break;
        if ((u64)(p + tag->size) > (u64)end)
            break;
        if (tag->type == ACPI_TAG_OLD_RSDP || tag->type == ACPI_TAG_NEW_RSDP)
            return p + sizeof(struct mb2_tag);
        p += (tag->size + 7u) & ~7u;
    }
    return scan_rsdp();
}

static const struct acpi_sdt_header *find_madt(const struct acpi_rsdp *rsdp)
{
    const struct acpi_sdt_header *root;
    u64 entries;

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        root = (const struct acpi_sdt_header *)(usize)rsdp->xsdt_address;
        entries = ((u64)root->length - sizeof(*root)) / 8;
        for (u64 i = 0; i < entries; ++i) {
            u64 addr = ((const u64 *)((const u8 *)root + sizeof(*root)))[i];
            const struct acpi_sdt_header *h =
                (const struct acpi_sdt_header *)(usize)addr;
            if (sig4(h->signature, "APIC"))
                return h;
        }
        return NULL;
    }

    if (rsdp->rsdt_address == 0)
        return NULL;
    root = (const struct acpi_sdt_header *)(usize)rsdp->rsdt_address;
    entries = ((u64)root->length - sizeof(*root)) / 4;
    u32 *table = (u32 *)((u8 *)root + sizeof(*root));
    for (u64 i = 0; i < entries; ++i) {
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header *)(usize)table[i];
        if (sig4(h->signature, "APIC"))
            return h;
    }
    return NULL;
}

static int add_cpu(u32 apic_id)
{
    for (u32 i = 0; i < info.cpu_count; ++i) {
        if (info.apic_ids[i] == apic_id)
            return 0;
    }
    if (info.cpu_count >= ACPI_MAX_CPUS)
        return 0;
    info.apic_ids[info.cpu_count++] = apic_id;
    return 1;
}

int acpi_init(u64 mb2_info)
{
    const u8 *rsdp_raw = find_rsdp(mb2_info);
    const struct acpi_rsdp *rsdp;
    const struct acpi_madt *madt;
    const u8 *p;
    const u8 *end;

    info.cpu_count = 0;
    info.bsp_apic_id = cpu_apic_id();
    info.lapic_base = 0xFEE00000ULL;

    if (rsdp_raw == NULL)
        goto fallback;
    rsdp = (const struct acpi_rsdp *)rsdp_raw;
    madt = (const struct acpi_madt *)find_madt(rsdp);
    if (madt == NULL)
        goto fallback;

    info.lapic_base = madt->local_apic_address;
    p = (const u8 *)madt + sizeof(struct acpi_madt);
    end = (const u8 *)madt + madt->header.length;
    while (p + 2 <= end) {
        u8 type = p[0];
        u8 len = p[1];

        if (len < 2 || p + len > end)
            break;
        if (type == 0 && len >= 8) {
            add_cpu(p[3]);
        } else if (type == 9 && len >= 16) {
            add_cpu(*(const u32 *)(p + 4));
        }
        p += len;
    }

    if (info.cpu_count == 0)
        goto fallback;
    return 0;

fallback:
    info.cpu_count = 0;
    add_cpu(info.bsp_apic_id);
    return 0;
}

const struct acpi_info *acpi_get(void)
{
    return &info;
}
