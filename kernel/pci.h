#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint32_t bar[6];
} pci_dev_t;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val);
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint8_t val);

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_dev_t* out);
void pci_enable_bus_master(pci_dev_t* dev);

#endif
