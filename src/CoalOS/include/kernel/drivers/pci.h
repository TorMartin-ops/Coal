/**
 * @file pci.h
 * @brief PCI configuration space access (mechanism #1) and device lookup on bus 0.
 */
#ifndef KERNEL_DRIVERS_PCI_H
#define KERNEL_DRIVERS_PCI_H

#include <kernel/core/types.h>

#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_IRQ_LINE    0x3C

#define PCI_CMD_MEMORY  0x2
#define PCI_CMD_MASTER  0x4

typedef struct pci_dev {
    uint8_t slot, func;
} pci_dev_t;

uint32_t pci_read32(const pci_dev_t *d, uint8_t reg);
void     pci_write32(const pci_dev_t *d, uint8_t reg, uint32_t value);

/** Finds the first function with this vendor/device ID. Returns true and fills *out. */
bool pci_find(uint16_t vendor, uint16_t device, pci_dev_t *out);

#endif
