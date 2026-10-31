/*
 * Lantest OS - PCI configuration space access (legacy 0xCF8/0xCFC)
 */
#pragma once

#include <types.h>

u32  pci_read32(u8 bus, u8 dev, u8 func, u8 off);
void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value);

/* Scan all buses/devices/functions for a vendor/device pair. */
int pci_find_device(u16 vendor, u16 device,
                    u8 *bus, u8 *dev, u8 *func);
