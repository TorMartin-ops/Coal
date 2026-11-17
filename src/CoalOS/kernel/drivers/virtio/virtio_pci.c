/**
 * @file virtio_pci.c
 * @brief virtio over PCI, modern interface (virtio 1.3, 4.1): the capabilities locate the
 *        common, notify, ISR and device configuration windows inside the BARs.
 */
#define LOG_MODULE "virtio-pci"

#include "virtqueue.h"
#include <kernel/core/log.h>
#include <kernel/drivers/pci.h>
#include <kernel/memory/dma.h>
#include <kernel/memory/kmalloc.h>

#define VIRTIO_PCI_VENDOR   0x1AF4
#define DEVICE_ID_FIRST     0x1000      /* transitional devices: the type is in the subsystem ID */
#define DEVICE_ID_MODERN    0x1040
#define DEVICE_ID_LAST      0x107F
#define PCI_SUBSYSTEM_ID    0x2C

/* virtio_pci_cap.cfg_type */
#define CAP_COMMON  1
#define CAP_NOTIFY  2
#define CAP_ISR     3
#define CAP_DEVICE  4

/* virtio_pci_cap, relative to the capability in config space */
#define CAP_CFG_TYPE   3
#define CAP_BAR        4
#define CAP_OFFSET     8
#define CAP_LENGTH     12
#define CAP_NOTIFY_MULT 16

/* Common configuration structure */
#define C_DEVICE_FEATURE_SEL 0x00
#define C_DEVICE_FEATURE     0x04
#define C_DRIVER_FEATURE_SEL 0x08
#define C_DRIVER_FEATURE     0x0C
#define C_DEVICE_STATUS      0x14
#define C_CONFIG_GENERATION  0x15
#define C_QUEUE_SELECT       0x16
#define C_QUEUE_SIZE         0x18
#define C_QUEUE_ENABLE       0x1C
#define C_QUEUE_NOTIFY_OFF   0x1E
#define C_QUEUE_DESC         0x20
#define C_QUEUE_DRIVER       0x28
#define C_QUEUE_DEVICE       0x30

typedef struct {
    volatile uint8_t *common, *isr, *device, *notify;
    uint32_t notify_mult, notify_len;
} pci_transport_t;

static pci_transport_t *tp(virtio_device_t *d) { return d->transport; }

static uint8_t pci_get_status(virtio_device_t *d) { return mmio_read8(tp(d)->common + C_DEVICE_STATUS); }
static void pci_set_status(virtio_device_t *d, uint8_t s) { mmio_write8(tp(d)->common + C_DEVICE_STATUS, s); }

static uint64_t pci_get_features(virtio_device_t *d) {
    volatile uint8_t *c = tp(d)->common;
    uint64_t f = 0;
    for (unsigned i = 0; i < 2; i++) {
        mmio_write32(c + C_DEVICE_FEATURE_SEL, i);
        f |= (uint64_t)mmio_read32(c + C_DEVICE_FEATURE) << (32 * i);
    }
    return f;
}

static void pci_set_features(virtio_device_t *d, uint64_t f) {
    volatile uint8_t *c = tp(d)->common;
    for (unsigned i = 0; i < 2; i++) {
        mmio_write32(c + C_DRIVER_FEATURE_SEL, i);
        mmio_write32(c + C_DRIVER_FEATURE, (uint32_t)(f >> (32 * i)));
    }
}

static void pci_get_config(virtio_device_t *d, unsigned off, void *buf, unsigned len) {
    pci_transport_t *t = tp(d);
    uint8_t gen;
    do {
        gen = mmio_read8(t->common + C_CONFIG_GENERATION);
        for (unsigned i = 0; i < len; i++) ((uint8_t *)buf)[i] = mmio_read8(t->device + off + i);
    } while (gen != mmio_read8(t->common + C_CONFIG_GENERATION));
}

static void pci_set_config(virtio_device_t *d, unsigned off, const void *buf, unsigned len) {
    for (unsigned i = 0; i < len; i++) mmio_write8(tp(d)->device + off + i, ((const uint8_t *)buf)[i]);
}

static unsigned pci_queue_max(virtio_device_t *d, unsigned index) {
    volatile uint8_t *c = tp(d)->common;
    mmio_write16(c + C_QUEUE_SELECT, (uint16_t)index);
    return mmio_read16(c + C_QUEUE_SIZE);
}

static int pci_queue_enable(virtio_device_t *d, virtqueue_t *vq) {
    pci_transport_t *t = tp(d);
    volatile uint8_t *c = t->common;

    mmio_write16(c + C_QUEUE_SELECT, (uint16_t)vq->index);
    mmio_write16(c + C_QUEUE_SIZE, (uint16_t)vq->num);
    mmio_write64(c + C_QUEUE_DESC, vq->desc_phys);
    mmio_write64(c + C_QUEUE_DRIVER, vq->avail_phys);
    mmio_write64(c + C_QUEUE_DEVICE, vq->used_phys);
    uint32_t off = mmio_read16(c + C_QUEUE_NOTIFY_OFF) * t->notify_mult;
    if (off + 2 > t->notify_len) return -1;
    vq->transport = (void *)(t->notify + off);
    mmio_write16(c + C_QUEUE_ENABLE, 1);
    return 0;
}

static void pci_notify(virtio_device_t *d, virtqueue_t *vq) {
    (void)d;
    mmio_write16(vq->transport, (uint16_t)vq->index);
}

static uint32_t pci_ack_irq(virtio_device_t *d) {
    return mmio_read8(tp(d)->isr) & 3u;     /* bit 0 queue, bit 1 config: the same as VIRTIO_IRQ_*; the read clears it */
}

static const virtio_transport_ops_t pci_ops = {
    pci_get_status, pci_set_status, pci_get_features, pci_set_features, pci_get_config, pci_set_config,
    pci_queue_max, pci_queue_enable, pci_notify, pci_ack_irq,
};

/* Maps the window described by the capability at `cap`; its length goes to *len when asked. */
static volatile uint8_t *map_cap(const pci_dev_t *p, uint8_t cap, uint32_t *len) {
    uint64_t bar;
    uint32_t off = pci_read32(p, cap + CAP_OFFSET), n = pci_read32(p, cap + CAP_LENGTH);

    if (!pci_bar_base(p, pci_read8(p, cap + CAP_BAR), &bar) || !n) return NULL;
    if (len) *len = n;
    return ioremap(bar + off, n);
}

/* The first capability of the given cfg_type, mapped. */
static volatile uint8_t *find_window(const pci_dev_t *p, uint8_t type, uint8_t *cap_out, uint32_t *len) {
    for (uint8_t cap = pci_find_cap(p, PCI_CAP_VENDOR, 0); cap; cap = pci_find_cap(p, PCI_CAP_VENDOR, cap)) {
        if (pci_read8(p, cap + CAP_CFG_TYPE) != type) continue;
        if (cap_out) *cap_out = cap;
        return map_cap(p, cap, len);
    }
    return NULL;
}

static bool probe_function(const pci_dev_t *p, uint16_t vendor, uint16_t device, void *arg) {
    (void)arg;
    if (vendor != VIRTIO_PCI_VENDOR || device < DEVICE_ID_FIRST || device > DEVICE_ID_LAST) return false;

    pci_transport_t t = { 0 };
    uint8_t notify_cap = 0;
    uint32_t type = device >= DEVICE_ID_MODERN ? (uint32_t)(device - DEVICE_ID_MODERN) : (uint32_t)(pci_read32(p, PCI_SUBSYSTEM_ID) >> 16);

    t.common = find_window(p, CAP_COMMON, NULL, NULL);
    t.isr = find_window(p, CAP_ISR, NULL, NULL);
    t.device = find_window(p, CAP_DEVICE, NULL, NULL);
    t.notify = find_window(p, CAP_NOTIFY, &notify_cap, &t.notify_len);
    if (!t.common || !t.isr || !t.notify || !t.device) return false;    /* a legacy-only device */
    t.notify_mult = pci_read32(p, notify_cap + CAP_NOTIFY_MULT);

    pci_write32(p, PCI_COMMAND, pci_read32(p, PCI_COMMAND) | PCI_CMD_MEMORY | PCI_CMD_MASTER);
    pci_transport_t *heap = kmalloc(sizeof(*heap));
    if (!heap) return false;
    *heap = t;
    uint8_t line = pci_read8(p, PCI_IRQ_LINE);
    virtio_device_t d = { .ops = &pci_ops, .id = type, .vendor = vendor, .irq = line < 16 ? line : -1, .transport = heap };
    if (!virtio_device_add(&d)) kfree(heap);
    return false;
}

void virtio_pci_init(void) {
    pci_scan(probe_function, NULL);
}
