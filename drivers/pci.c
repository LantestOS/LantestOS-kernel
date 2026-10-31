/*
 * Lantest OS - PCI configuration space access
 *
 * Uses the legacy x86 mechanism: write address to 0xCF8, read/write data at
 * 0xCFC. Enough for QEMU's i440fx machine and most BIOS-era hardware.
 */
#include <types.h>
#include <io.h>
#include <pci.h>

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static u32 config_address(u8 bus, u8 dev, u8 func, u8 off)
{
    return 0x80000000u |
           ((u32)bus << 16) |
           ((u32)dev << 11) |
           ((u32)func << 8) |
           ((u32)off & 0xFCu);
}

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 off)
{
    outl(PCI_CONFIG_ADDR, config_address(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value)
{
    outl(PCI_CONFIG_ADDR, config_address(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(u16 vendor, u16 device,
                    u8 *bus, u8 *dev, u8 *func)
{
    for (u32 b = 0; b < 256; ++b) {
        for (u32 d = 0; d < 32; ++d) {
            for (u32 f = 0; f < 8; ++f) {
                u32 id = pci_read32((u8)b, (u8)d, (u8)f, 0);

                if ((id & 0xFFFFu) == 0xFFFFu)
                    continue;
                if ((id & 0xFFFFu) == vendor &&
                    ((id >> 16) & 0xFFFFu) == device) {
                    if (bus != NULL)
                        *bus = (u8)b;
                    if (dev != NULL)
                        *dev = (u8)d;
                    if (func != NULL)
                        *func = (u8)f;
                    return 1;
                }
            }
        }
    }

    return 0;
}
