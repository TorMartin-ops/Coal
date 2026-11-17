/**
 * @file pci.h
 * @brief PCI configuration space access (mechanism #1) and device lookup on bus 0.
 */
#ifndef KERNEL_DRIVERS_PCI_H
#define KERNEL_DRIVERS_PCI_H

#include <kernel/core/types.h>

#define PCI_COMMAND     0x04
#define PCI_STATUS      0x06
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_IRQ_LINE    0x3C
#define PCI_STATUS_CAPS 0x10
#define PCI_CAP_VENDOR  0x09    /* vendor specific capability (virtio uses it) */

#define PCI_CMD_MEMORY  0x2
#define PCI_CMD_MASTER  0x4

typedef struct pci_dev {
    uint8_t slot, func;
} pci_dev_t;

uint32_t pci_read32(const pci_dev_t *d, uint8_t reg);
void     pci_write32(const pci_dev_t *d, uint8_t reg, uint32_t value);

uint8_t  pci_read8(const pci_dev_t *d, uint8_t reg);
uint16_t pci_read16(const pci_dev_t *d, uint8_t reg);

/** Calls visit() for every function on bus 0 until it returns true. */
void pci_scan(bool (*visit)(const pci_dev_t *d, uint16_t vendor, uint16_t device, void *arg), void *arg);

/** Finds the first function with this vendor/device ID. Returns true and fills *out. */
bool pci_find(uint16_t vendor, uint16_t device, pci_dev_t *out);

/** Same for the nth match (from 0). */
bool pci_find_nth(uint16_t vendor, uint16_t device, unsigned nth, pci_dev_t *out);

/** Config offset of the first capability with this ID at or after the list position `from` (0: start of the list); 0 if none. */
uint8_t pci_find_cap(const pci_dev_t *d, uint8_t id, uint8_t from);

/** Physical base address of memory BAR `index`, following 64-bit pairs; false for an I/O or absent BAR. */
bool pci_bar_base(const pci_dev_t *d, unsigned index, uint64_t *base);

#endif
