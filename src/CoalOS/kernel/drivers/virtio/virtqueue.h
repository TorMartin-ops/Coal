/**
 * @file virtqueue.h
 * @brief Split virtqueue layout (virtio 1.3, 2.7) and the queue state the transports need.
 */
#ifndef VIRTIO_VIRTQUEUE_H
#define VIRTIO_VIRTQUEUE_H

#include <kernel/drivers/virtio/virtio.h>

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2           /* the device writes this buffer */
#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VRING_USED_F_NO_NOTIFY     1

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vring_avail {
    uint16_t flags, idx;
    uint16_t ring[];                    /* num entries, then used_event */
};

struct vring_used_elem {
    uint32_t id, len;
};

struct vring_used {
    uint16_t flags, idx;
    struct vring_used_elem ring[];      /* num entries, then avail_event */
};

struct virtqueue {
    virtio_device_t *vdev;
    unsigned index, num;                /* num is a power of two */
    void (*cb)(virtqueue_t *);
    void *priv;
    void *transport;                    /* transport private (the notify address) */

    uint64_t desc_phys, avail_phys, used_phys;

    volatile struct vring_desc  *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used  *used;
    void **cookie;                      /* per head descriptor */

    uint16_t free_head, nfree;
    uint16_t avail_idx;                 /* next avail slot (shadow of avail->idx) */
    uint16_t kicked_idx;                /* avail_idx the device was last told about */
    uint16_t last_used;
    bool     event_idx;
};

#endif
