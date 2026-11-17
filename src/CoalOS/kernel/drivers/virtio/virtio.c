/**
 * @file virtio.c
 * @brief virtio core: device list, feature negotiation, interrupt dispatch.
 *
 * Devices on one PIC line share a handler that asks each of them whether it interrupted. A line
 * that already has a handler (the e1000) keeps it: we call it after ours, and it sends the EOI.
 * A device without a usable line (PCI INTx routed past the PIC) is polled by a 1 ms timer.
 */
#define LOG_MODULE "virtio"

#include "virtqueue.h"
#include <kernel/core/log.h>
#include <kernel/cpu/idt.h>
#include <kernel/drivers/pic.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define MAX_DEVICES   16
#define RESET_SPINS   1000000u
#define POLL_MS       1

extern const virtio_driver_t virtio_blk_driver, virtio_console_driver, virtio_rng_driver, virtio_vsock_driver;

static const virtio_driver_t *const g_drivers[] = {
    &virtio_blk_driver, &virtio_console_driver, &virtio_rng_driver, &virtio_vsock_driver,
};

static virtio_device_t *g_devices[MAX_DEVICES];
static int_handler_t    g_prev_handler[16];
static bool             g_line_hooked[16];
static struct ktimer    g_poll_timer;
static bool             g_polling;

/* ---- Interrupts --------------------------------------------------------------------- */

static void service(virtio_device_t *d) {
    uint32_t cause = d->ops->ack_irq(d);

    if ((cause & VIRTIO_IRQ_CONFIG) && d->driver && d->driver->config_changed) d->driver->config_changed(d);
    if (!(cause & VIRTIO_IRQ_VRING)) return;
    for (unsigned i = 0; i < d->nvqs; i++) {
        virtqueue_t *vq = d->vqs[i];
        if (vq && vq->cb && vq_pending(vq)) vq->cb(vq);
    }
}

static void line_irq(isr_frame_t *frame) {
    unsigned irq = (unsigned)(frame->int_no - IRQ0_VECTOR);

    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i] && g_devices[i]->irq == (int)irq) service(g_devices[i]);
    if (g_prev_handler[irq]) g_prev_handler[irq](frame);
    else pic_send_eoi(irq);
}

static void poll_tick(struct ktimer *t) {
    for (unsigned i = 0; i < MAX_DEVICES; i++)
        if (g_devices[i] && g_devices[i]->irq < 0) service(g_devices[i]);
    timer_add(t, POLL_MS);
}

static void attach_irq(const virtio_device_t *d) {
    if (d->irq < 0 || d->irq >= 16) {
        if (!g_polling) {
            g_polling = true;
            g_poll_timer.fn = poll_tick;
            timer_add(&g_poll_timer, POLL_MS);
        }
        return;
    }
    if (!g_line_hooked[d->irq]) {
        g_line_hooked[d->irq] = true;
        g_prev_handler[d->irq] = get_int_handler(IRQ0_VECTOR + d->irq);
        register_int_handler(IRQ0_VECTOR + d->irq, line_irq, NULL);
        pic_unmask((unsigned)d->irq);
    }
}

/* ---- Device setup --------------------------------------------------------------------- */

static void set_status_bit(virtio_device_t *d, uint8_t bit) {
    d->ops->set_status(d, d->ops->get_status(d) | bit);
}

static void reset(virtio_device_t *d) {
    d->ops->set_status(d, 0);
    for (unsigned i = 0; i < RESET_SPINS && d->ops->get_status(d); i++) asm volatile("pause");
}

/* Reset, acknowledge, agree on features. False if the device cannot be driven. */
static bool negotiate(virtio_device_t *d, const virtio_driver_t *drv) {
    reset(d);
    set_status_bit(d, VIRTIO_STATUS_ACK);
    set_status_bit(d, VIRTIO_STATUS_DRIVER);

    uint64_t offered = d->ops->get_features(d);
    if (!(offered & VIRTIO_FEATURE(VIRTIO_F_VERSION_1))) {
        LOG_WARN("%s: device is legacy only", drv->name);
        return false;
    }
    d->features = offered & (drv->features | VIRTIO_FEATURE(VIRTIO_F_VERSION_1) | VIRTIO_FEATURE(VIRTIO_F_EVENT_IDX));
    d->ops->set_features(d, d->features);
    set_status_bit(d, VIRTIO_STATUS_FEATURES_OK);
    if (!(d->ops->get_status(d) & VIRTIO_STATUS_FEATURES_OK)) {
        LOG_WARN("%s: device rejected the features", drv->name);
        return false;
    }
    return true;
}

void virtio_device_ready(virtio_device_t *d) {
    set_status_bit(d, VIRTIO_STATUS_DRIVER_OK);
}

bool virtio_device_add(const virtio_device_t *dev) {
    const virtio_driver_t *drv = NULL;
    unsigned slot = 0;

    for (unsigned i = 0; i < sizeof(g_drivers) / sizeof(g_drivers[0]); i++)
        if (g_drivers[i]->id == dev->id) drv = g_drivers[i];
    while (slot < MAX_DEVICES && g_devices[slot]) slot++;
    if (!drv || slot == MAX_DEVICES) return false;

    virtio_device_t *d = kmalloc(sizeof(*d));
    if (!d) return false;
    *d = *dev;
    d->driver = drv;
    attach_irq(d);
    g_devices[slot] = d;

    bool ok = negotiate(d, drv) && drv->probe(d) == 0;
    if (!ok) {
        reset(d);                           /* stop it from interrupting or touching its rings */
        d->ops->set_status(d, VIRTIO_STATUS_FAILED);
        g_devices[slot] = NULL;
        kfree(d);
        return false;
    }
    return true;
}

void virtio_init(void) {
    virtio_pci_init();
    virtio_mmio_init();
}
