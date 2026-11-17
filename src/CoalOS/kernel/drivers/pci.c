#include <kernel/drivers/pci.h>
#include <kernel/lib/port_io.h>

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static void select_reg(const pci_dev_t *d, uint8_t reg) {
    outl(CONFIG_ADDRESS, 0x80000000u | ((uint32_t)d->slot << 11) | ((uint32_t)d->func << 8) | (reg & 0xFC));
}

uint32_t pci_read32(const pci_dev_t *d, uint8_t reg) {
    select_reg(d, reg);
    return inl(CONFIG_DATA);
}

void pci_write32(const pci_dev_t *d, uint8_t reg, uint32_t value) {
    select_reg(d, reg);
    outl(CONFIG_DATA, value);
}

bool pci_find(uint16_t vendor, uint16_t device, pci_dev_t *out) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            pci_dev_t d = { slot, func };
            uint32_t id = pci_read32(&d, 0);
            if (id == 0xFFFFFFFFu) {
                if (func == 0) break;               /* empty slot */
                continue;
            }
            if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                *out = d;
                return true;
            }
            if (func == 0 && !(pci_read32(&d, 0x0C) & 0x800000)) break;   /* single-function device */
        }
    }
    return false;
}
