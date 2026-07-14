#include "pci.h"
#include <stdint.h>

static uint32_t make_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    return 0x80000000
         | ((uint32_t)bus << 16)
         | ((uint32_t)dev << 11)
         | ((uint32_t)func << 8)
         | (reg & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    __asm__ volatile("outl %0, %w1" : : "a"(make_addr(bus, dev, func, reg)), "d"((uint16_t)PCI_CONFIG_ADDR));
    uint32_t val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "d"((uint16_t)PCI_CONFIG_DATA));
    return val;
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t val = pci_read32(bus, dev, func, reg);
    return (val >> ((reg & 2) * 8)) & 0xFFFF;
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t val = pci_read32(bus, dev, func, reg);
    return (val >> ((reg & 3) * 8)) & 0xFF;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val) {
    __asm__ volatile("outl %0, %w1" : : "a"(make_addr(bus, dev, func, reg)), "d"((uint16_t)PCI_CONFIG_ADDR));
    __asm__ volatile("outl %0, %w1" : : "a"(val), "d"((uint16_t)PCI_CONFIG_DATA));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val) {
    uint32_t cur = pci_read32(bus, dev, func, reg);
    uint32_t mask = 0xFFFF << ((reg & 2) * 8);
    pci_write32(bus, dev, func, reg, (cur & ~mask) | ((uint32_t)val << ((reg & 2) * 8)));
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint8_t val) {
    uint32_t cur = pci_read32(bus, dev, func, reg);
    uint32_t mask = 0xFF << ((reg & 3) * 8);
    pci_write32(bus, dev, func, reg, (cur & ~mask) | ((uint32_t)val << ((reg & 3) * 8)));
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_dev_t* out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t vd = pci_read32((uint8_t)bus, dev, 0, 0);
            if ((vd & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = pci_read8((uint8_t)bus, dev, 0, 0x0E);
            uint8_t funcs = (hdr & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < funcs; func++) {
                uint32_t vdf = pci_read32((uint8_t)bus, dev, func, 0);
                if ((vdf & 0xFFFF) == 0xFFFF) continue;
                if ((vdf & 0xFFFF) == vendor_id && ((vdf >> 16) & 0xFFFF) == device_id) {
                    if (out) {
                        out->bus = (uint8_t)bus;
                        out->dev = dev;
                        out->func = func;
                        out->vendor_id = vendor_id;
                        out->device_id = device_id;
                        uint32_t classrev = pci_read32((uint8_t)bus, dev, func, 0x08);
                        out->class_code = (classrev >> 24) & 0xFF;
                        out->subclass = (classrev >> 16) & 0xFF;
                        out->prog_if = (classrev >> 8) & 0xFF;
                        out->revision = classrev & 0xFF;
                        for (int i = 0; i < 6; i++) {
                            out->bar[i] = pci_read32((uint8_t)bus, dev, func, 0x10 + i * 4);
                        }
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

void pci_enable_bus_master(pci_dev_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= 0x04; // bus master
    cmd |= 0x02; // memory space
    pci_write16(dev->bus, dev->dev, dev->func, 0x04, cmd);
}
