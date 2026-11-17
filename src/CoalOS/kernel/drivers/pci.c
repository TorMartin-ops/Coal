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

uint8_t pci_read8(const pci_dev_t *d, uint8_t reg) {
    return (uint8_t)(pci_read32(d, reg) >> (8 * (reg & 3)));
}

uint16_t pci_read16(const pci_dev_t *d, uint8_t reg) {
    return (uint16_t)(pci_read32(d, reg) >> (8 * (reg & 2)));
}

void pci_scan(bool (*visit)(const pci_dev_t *d, uint16_t vendor, uint16_t device, void *arg), void *arg) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            pci_dev_t d = { slot, func };
            uint32_t id = pci_read32(&d, 0);
            if (id == 0xFFFFFFFFu) {
                if (func == 0) break;               /* empty slot */
                continue;
            }
            if (visit(&d, (uint16_t)(id & 0xFFFF), (uint16_t)(id >> 16), arg)) return;
            if (func == 0 && !(pci_read32(&d, 0x0C) & 0x800000)) break;   /* single-function device */
        }
    }
}

struct find_args { uint16_t vendor, device; unsigned nth; pci_dev_t *out; bool found; };

static bool find_visit(const pci_dev_t *d, uint16_t vendor, uint16_t device, void *arg) {
    struct find_args *a = arg;
    if (vendor != a->vendor || device != a->device || a->nth--) return false;
    *a->out = *d;
    return a->found = true;
}

bool pci_find_nth(uint16_t vendor, uint16_t device, unsigned nth, pci_dev_t *out) {
    struct find_args a = { vendor, device, nth, out, false };
    pci_scan(find_visit, &a);
    return a.found;
}

bool pci_find(uint16_t vendor, uint16_t device, pci_dev_t *out) {
    return pci_find_nth(vendor, device, 0, out);
}

uint8_t pci_find_cap(const pci_dev_t *d, uint8_t id, uint8_t from) {
    if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAPS)) return 0;
    uint8_t pos = (from ? pci_read8(d, from + 1) : pci_read8(d, PCI_CAP_PTR)) & 0xFC;
    for (unsigned guard = 0; pos >= 0x40 && guard < 48; guard++, pos = pci_read8(d, pos + 1) & 0xFC)
        if (pci_read8(d, pos) == id) return pos;
    return 0;
}

bool pci_bar_base(const pci_dev_t *d, unsigned index, uint64_t *base) {
    uint8_t reg = (uint8_t)(PCI_BAR0 + 4 * index);
    uint32_t lo = pci_read32(d, reg);
    if (index > 5 || (lo & 1)) return false;
    uint64_t b = lo & ~0xFu;
    if ((lo & 6) == 4 && index < 5) b |= (uint64_t)pci_read32(d, reg + 4) << 32;
    *base = b;
    return b != 0;
}
