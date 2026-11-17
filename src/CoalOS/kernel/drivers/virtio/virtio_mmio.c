/**
 * @file virtio_mmio.c
 * @brief virtio over memory-mapped registers, version 2 (virtio 1.3, 4.2).
 *
 * There is no bus to scan: devices come from the kernel command line, one word each, in the
 * form Linux, Firecracker and QEMU's microvm use:
 *   virtio_mmio.device=<size>@<base>:<irq>      size accepts K and M, base and irq are decimal or 0x hex
 */
#define LOG_MODULE "virtio-mmio"

#include "virtqueue.h"
#include <kernel/core/cmdline.h>
#include <kernel/core/log.h>
#include <kernel/memory/dma.h>
#include <kernel/memory/kmalloc.h>

#define MAGIC 0x74726976u               /* "virt" */

#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICE_ID    0x008
#define R_VENDOR_ID    0x00C
#define R_DEVICE_FEAT  0x010
#define R_DEVICE_FSEL  0x014
#define R_DRIVER_FEAT  0x020
#define R_DRIVER_FSEL  0x024
#define R_QUEUE_SEL    0x030
#define R_QUEUE_MAX    0x034
#define R_QUEUE_NUM    0x038
#define R_QUEUE_READY  0x044
#define R_QUEUE_NOTIFY 0x050
#define R_IRQ_STATUS   0x060
#define R_IRQ_ACK      0x064
#define R_STATUS       0x070
#define R_QUEUE_DESC   0x080
#define R_QUEUE_DRIVER 0x090
#define R_QUEUE_DEVICE 0x0A0
#define R_CONFIG_GEN   0x0FC
#define R_CONFIG       0x100

#define REGION_MIN 0x200

static volatile uint8_t *regs(virtio_device_t *d) { return d->transport; }

static uint8_t mm_get_status(virtio_device_t *d) { return (uint8_t)mmio_read32(regs(d) + R_STATUS); }
static void mm_set_status(virtio_device_t *d, uint8_t s) { mmio_write32(regs(d) + R_STATUS, s); }

static uint64_t mm_get_features(virtio_device_t *d) {
    uint64_t f = 0;
    for (unsigned i = 0; i < 2; i++) {
        mmio_write32(regs(d) + R_DEVICE_FSEL, i);
        f |= (uint64_t)mmio_read32(regs(d) + R_DEVICE_FEAT) << (32 * i);
    }
    return f;
}

static void mm_set_features(virtio_device_t *d, uint64_t f) {
    for (unsigned i = 0; i < 2; i++) {
        mmio_write32(regs(d) + R_DRIVER_FSEL, i);
        mmio_write32(regs(d) + R_DRIVER_FEAT, (uint32_t)(f >> (32 * i)));
    }
}

static void mm_get_config(virtio_device_t *d, unsigned off, void *buf, unsigned len) {
    uint32_t gen;
    do {
        gen = mmio_read32(regs(d) + R_CONFIG_GEN);
        for (unsigned i = 0; i < len; i++) ((uint8_t *)buf)[i] = mmio_read8(regs(d) + R_CONFIG + off + i);
    } while (gen != mmio_read32(regs(d) + R_CONFIG_GEN));
}

static void mm_set_config(virtio_device_t *d, unsigned off, const void *buf, unsigned len) {
    for (unsigned i = 0; i < len; i++) mmio_write8(regs(d) + R_CONFIG + off + i, ((const uint8_t *)buf)[i]);
}

static unsigned mm_queue_max(virtio_device_t *d, unsigned index) {
    mmio_write32(regs(d) + R_QUEUE_SEL, index);
    return mmio_read32(regs(d) + R_QUEUE_READY) ? 0 : mmio_read32(regs(d) + R_QUEUE_MAX);
}

static int mm_queue_enable(virtio_device_t *d, virtqueue_t *vq) {
    volatile uint8_t *r = regs(d);

    mmio_write32(r + R_QUEUE_SEL, vq->index);
    mmio_write32(r + R_QUEUE_NUM, vq->num);
    mmio_write64(r + R_QUEUE_DESC, vq->desc_phys);
    mmio_write64(r + R_QUEUE_DRIVER, vq->avail_phys);
    mmio_write64(r + R_QUEUE_DEVICE, vq->used_phys);
    mmio_write32(r + R_QUEUE_READY, 1);
    return 0;
}

static void mm_notify(virtio_device_t *d, virtqueue_t *vq) {
    mmio_write32(regs(d) + R_QUEUE_NOTIFY, vq->index);
}

/* Acknowledge before the queues are looked at: a completion that lands meanwhile raises the line again. */
static uint32_t mm_ack_irq(virtio_device_t *d) {
    uint32_t st = mmio_read32(regs(d) + R_IRQ_STATUS) & 3u;     /* bit 0 used buffers, bit 1 config: the VIRTIO_IRQ_* bits */
    if (st) mmio_write32(regs(d) + R_IRQ_ACK, st);
    return st;
}

static const virtio_transport_ops_t mmio_ops = {
    mm_get_status, mm_set_status, mm_get_features, mm_set_features, mm_get_config, mm_set_config,
    mm_queue_max, mm_queue_enable, mm_notify, mm_ack_irq,
};

/* "<size>@<base>:<irq>" */
static bool parse_device(const char *s, uint64_t *size, uint64_t *base, uint64_t *irq) {
    const char *end;

    if (!cmdline_number(s, &end, size)) return false;
    if (*end == 'K' || *end == 'k') { *size <<= 10; end++; }
    else if (*end == 'M' || *end == 'm') { *size <<= 20; end++; }
    if (*end++ != '@' || !cmdline_number(end, &end, base)) return false;
    return *end++ == ':' && cmdline_number(end, &end, irq);     /* an optional ":<id>" after it is ignored */
}

void virtio_mmio_init(void) {
    char word[64];

    for (unsigned n = 0; cmdline_get("virtio_mmio.device", n, word, sizeof(word)); n++) {
        uint64_t size, base, irq;
        if (!parse_device(word, &size, &base, &irq)) {
            LOG_WARN("bad virtio_mmio.device=%s", word);
            continue;
        }
        volatile uint8_t *r = ioremap(base, MAX(size, (uint64_t)REGION_MIN));
        if (!r || mmio_read32(r + R_MAGIC) != MAGIC) { LOG_WARN("no virtio device at %#x", (unsigned)base); continue; }
        if (mmio_read32(r + R_VERSION) != 2) { LOG_WARN("%#x: only virtio-mmio version 2 is supported", (unsigned)base); continue; }
        uint32_t id = mmio_read32(r + R_DEVICE_ID);
        if (!id) continue;                                      /* an empty slot */

        virtio_device_t d = { .ops = &mmio_ops, .id = id, .vendor = mmio_read32(r + R_VENDOR_ID),
                              .irq = irq < 16 ? (int)irq : -1, .transport = (void *)r };
        virtio_device_add(&d);
    }
}
