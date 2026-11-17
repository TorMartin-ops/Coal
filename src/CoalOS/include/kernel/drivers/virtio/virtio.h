/**
 * @file virtio.h
 * @brief virtio 1.x core: devices, feature negotiation, split virtqueues and the driver interface.
 *
 * Only the modern ("version 1") interface is supported. A transport (virtio-pci, virtio-mmio)
 * finds devices and implements virtio_transport_ops_t; drivers (blk, console, rng, vsock) see
 * a virtio_device_t and its queues and never touch the transport.
 *
 * Locking: this kernel runs on one CPU and protects shared state by switching interrupts
 * off. All virtqueue calls must be made with interrupts off (queue callbacks already run
 * that way); a driver brackets its submit paths with local_irq_save()/local_irq_restore().
 */
#ifndef KERNEL_DRIVERS_VIRTIO_H
#define KERNEL_DRIVERS_VIRTIO_H

#include <kernel/core/types.h>

/* Device types (virtio 1.3, section 5) */
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_RNG     4
#define VIRTIO_ID_VSOCK   19

/* Device status bits */
#define VIRTIO_STATUS_ACK         0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED      0x80

/* Transport and ring feature bits */
#define VIRTIO_F_EVENT_IDX   29
#define VIRTIO_F_VERSION_1   32
#define VIRTIO_FEATURE(bit)  (1ull << (bit))

/* Bits returned by the transport's ack_irq() */
#define VIRTIO_IRQ_VRING  1u
#define VIRTIO_IRQ_CONFIG 2u

#define VIRTIO_MAX_QUEUES 4

typedef struct virtio_device virtio_device_t;
typedef struct virtqueue virtqueue_t;

/** What a transport provides. Features and registers are little endian; 64-bit values are split in two 32-bit accesses. */
typedef struct virtio_transport_ops {
    uint8_t  (*get_status)(virtio_device_t *);
    void     (*set_status)(virtio_device_t *, uint8_t);
    uint64_t (*get_features)(virtio_device_t *);
    void     (*set_features)(virtio_device_t *, uint64_t);
    /** Reads device config bytes; the transport retries until the config generation is stable. */
    void     (*get_config)(virtio_device_t *, unsigned off, void *buf, unsigned len);
    void     (*set_config)(virtio_device_t *, unsigned off, const void *buf, unsigned len);
    /** Largest queue the device offers at this index, 0 if there is none. */
    unsigned (*queue_max)(virtio_device_t *, unsigned index);
    /** Programs vq->index, vq->num and the three ring addresses and enables the queue. */
    int      (*queue_enable)(virtio_device_t *, virtqueue_t *);
    void     (*notify)(virtio_device_t *, virtqueue_t *);
    /** Reads and acknowledges the interrupt cause: VIRTIO_IRQ_* bits, 0 if this device did not interrupt. */
    uint32_t (*ack_irq)(virtio_device_t *);
} virtio_transport_ops_t;

/** A driver for one device type. */
typedef struct virtio_driver {
    const char *name;
    uint32_t    id;                         /* VIRTIO_ID_* */
    uint64_t    features;                   /* optional features to accept besides VERSION_1 and EVENT_IDX */
    /** Sets up queues and state, then calls virtio_device_ready(). A negative return fails the device. */
    int       (*probe)(virtio_device_t *);
    void      (*config_changed)(virtio_device_t *);   /* may be NULL; runs in interrupt context */
} virtio_driver_t;

struct virtio_device {
    const virtio_transport_ops_t *ops;
    const virtio_driver_t *driver;
    uint32_t   id, vendor;
    uint64_t   features;                    /* negotiated */
    int        irq;                         /* PIC line, -1: no usable line, polled */
    unsigned   nvqs;
    virtqueue_t *vqs[VIRTIO_MAX_QUEUES];
    void      *transport;                   /* transport private */
    void      *priv;                        /* driver private */
};

/* ---- Transports: report a device they found ------------------------------------- */

/** Probes the device with the matching driver. `dev` is copied; returns false when nothing took it. */
bool virtio_device_add(const virtio_device_t *dev);

/** Starts the transports and probes every device they find. Call after the PIC, timers and memory are up. */
void virtio_init(void);

/* ---- Drivers -------------------------------------------------------------------- */

static inline bool virtio_has(const virtio_device_t *d, unsigned bit) { return (d->features >> bit) & 1; }

/** Reads `len` bytes of device configuration at `off`. */
static inline void virtio_read_config(virtio_device_t *d, unsigned off, void *buf, unsigned len) {
    d->ops->get_config(d, off, buf, len);
}

/** Writes `len` bytes of device configuration at `off`. */
static inline void virtio_write_config(virtio_device_t *d, unsigned off, const void *buf, unsigned len) {
    d->ops->set_config(d, off, buf, len);
}

/** Last step of a driver's probe: the device is live from here (DRIVER_OK). */
void virtio_device_ready(virtio_device_t *d);

/** A buffer for a virtqueue: a kernel direct-mapped address and its length. */
typedef struct {
    const void *addr;
    uint32_t    len;
} vq_sg_t;

/**
 * Creates queue `index` with at most `max_size` entries (a power of two) and registers `cb`, which runs
 * in interrupt context when buffers have been used. NULL if the device lacks the queue.
 */
virtqueue_t *virtio_queue_create(virtio_device_t *d, unsigned index, unsigned max_size, void (*cb)(virtqueue_t *));

/**
 * Queues one buffer chain: sg[0..nout) the device reads, sg[nout..nout+nin) it writes. `cookie` comes back
 * from vq_get_buf(). Returns 0 or -ENOSPC when the ring is full. Does not notify; see vq_kick().
 */
int vq_add(virtqueue_t *vq, const vq_sg_t *sg, unsigned nout, unsigned nin, void *cookie);

/** Makes the queued chains visible and notifies the device unless it asked not to be. */
void vq_kick(virtqueue_t *vq);

/** Next completed chain: its cookie, and *len bytes written by the device. NULL if none. */
void *vq_get_buf(virtqueue_t *vq, uint32_t *len);

/** Free descriptors left. A chain of n buffers needs n. */
unsigned vq_free(const virtqueue_t *vq);

/** Chains the device has completed that vq_get_buf() has not returned yet. */
bool vq_pending(const virtqueue_t *vq);

/** Interrupt suppression: callbacks may stay off while a driver polls; enabling returns true if work arrived meanwhile. */
void vq_disable_cb(virtqueue_t *vq);
bool vq_enable_cb(virtqueue_t *vq);

void  vq_set_priv(virtqueue_t *vq, void *priv);
void *vq_priv(const virtqueue_t *vq);
virtio_device_t *vq_device(const virtqueue_t *vq);

/* ---- Transports provide these ---------------------------------------------------- */

void virtio_pci_init(void);
void virtio_mmio_init(void);

#endif
