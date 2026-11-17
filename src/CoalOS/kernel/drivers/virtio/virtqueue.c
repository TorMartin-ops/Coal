/**
 * @file virtqueue.c
 * @brief Split virtqueues: descriptor chains, kick suppression (flags or event index), completion.
 */
#include "virtqueue.h"
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/barrier.h>
#include <kernel/memory/dma.h>
#include <kernel/lib/string.h>
#include <kernel/memory/kmalloc.h>

#define used_event(vq)  ((vq)->avail->ring[(vq)->num])
#define avail_event(vq) (*(volatile uint16_t *)&(vq)->used->ring[(vq)->num])

/* True if the device, last told about `old`, has to hear that the index moved to `new_idx` (virtio 1.3, 2.7.10). */
static inline bool need_event(uint16_t event, uint16_t new_idx, uint16_t old) {
    return (uint16_t)(new_idx - event - 1) < (uint16_t)(new_idx - old);
}

static size_t ring_bytes(unsigned num, size_t *avail_off, size_t *used_off) {
    size_t desc = sizeof(struct vring_desc) * num;
    size_t avail = ALIGN_UP(desc + 4 + 2 * num + 2, 4);
    *avail_off = desc;
    *used_off = avail;
    return avail + 4 + sizeof(struct vring_used_elem) * num + 2;
}

virtqueue_t *virtio_queue_create(virtio_device_t *d, unsigned index, unsigned max_size, void (*cb)(virtqueue_t *)) {
    unsigned max = d->ops->queue_max(d, index);
    if (!max || index >= VIRTIO_MAX_QUEUES || d->vqs[index]) return NULL;

    unsigned num = 1;
    while (num * 2 <= MIN(max, max_size)) num *= 2;     /* the largest power of two both sides allow */

    virtqueue_t *vq = kmalloc(sizeof(*vq));
    if (!vq) return NULL;
    memset(vq, 0, sizeof(*vq));
    vq->cookie = kmalloc(sizeof(void *) * num);
    size_t avail_off, used_off, size = ring_bytes(num, &avail_off, &used_off);
    uintptr_t phys;
    uint8_t *mem = dma_alloc_coherent(size, &phys);
    if (!vq->cookie || !mem) {
        kfree(vq->cookie);
        dma_free_coherent(mem);
        kfree(vq);
        return NULL;
    }
    vq->vdev = d;
    vq->index = index;
    vq->num = num;
    vq->cb = cb;
    vq->desc = (volatile struct vring_desc *)mem;
    vq->avail = (volatile struct vring_avail *)(mem + avail_off);
    vq->used = (volatile struct vring_used *)(mem + used_off);
    vq->desc_phys = phys;
    vq->avail_phys = phys + avail_off;
    vq->used_phys = phys + used_off;
    vq->event_idx = virtio_has(d, VIRTIO_F_EVENT_IDX);
    vq->nfree = (uint16_t)num;
    for (unsigned i = 0; i < num; i++) vq->desc[i].next = (uint16_t)(i + 1);

    if (d->ops->queue_enable(d, vq) != 0) {
        kfree(vq->cookie);
        dma_free_coherent(mem);
        kfree(vq);
        return NULL;
    }
    d->vqs[index] = vq;
    if (index >= d->nvqs) d->nvqs = index + 1;
    return vq;
}

int vq_add(virtqueue_t *vq, const vq_sg_t *sg, unsigned nout, unsigned nin, void *cookie) {
    unsigned n = nout + nin;
    if (!n || n > vq->nfree) return -ENOSPC;

    uint16_t head = vq->free_head, i = head;
    for (unsigned k = 0; k < n; k++) {
        volatile struct vring_desc *d = &vq->desc[i];
        d->addr = dma_phys(sg[k].addr);
        d->len = sg[k].len;
        d->flags = (uint16_t)((k >= nout ? VRING_DESC_F_WRITE : 0) | (k + 1 < n ? VRING_DESC_F_NEXT : 0));
        if (k + 1 < n) i = d->next;
    }
    vq->free_head = vq->desc[i].next;
    vq->nfree = (uint16_t)(vq->nfree - n);
    vq->cookie[head] = cookie;

    vq->avail->ring[vq->avail_idx & (vq->num - 1)] = head;
    vq->avail_idx++;
    return 0;
}

void vq_kick(virtqueue_t *vq) {
    uint16_t old = vq->kicked_idx, now = vq->avail_idx;
    if (old == now) return;
    wmb();                                  /* ring entries and descriptors before the index */
    vq->avail->idx = now;
    vq->kicked_idx = now;
    mb();                                   /* the index before the device's "do not notify" state is read */
    bool notify = vq->event_idx ? need_event(avail_event(vq), now, old)
                                : !(vq->used->flags & VRING_USED_F_NO_NOTIFY);
    if (notify) vq->vdev->ops->notify(vq->vdev, vq);
}

bool vq_pending(const virtqueue_t *vq) {
    return vq->last_used != vq->used->idx;
}

void *vq_get_buf(virtqueue_t *vq, uint32_t *len) {
    if (!vq_pending(vq)) return NULL;
    rmb();                                  /* the index before the element it covers */
    volatile struct vring_used_elem *e = &vq->used->ring[vq->last_used & (vq->num - 1)];
    uint16_t head = (uint16_t)e->id;
    if (head >= vq->num) return NULL;       /* a broken device: ignore the entry */
    if (len) *len = e->len;
    vq->last_used++;
    if (vq->event_idx) used_event(vq) = vq->last_used;      /* interrupt for the next completion */

    void *cookie = vq->cookie[head];
    uint16_t i = head;
    unsigned n = 1;
    while (vq->desc[i].flags & VRING_DESC_F_NEXT) { i = vq->desc[i].next; n++; }
    vq->desc[i].next = vq->free_head;
    vq->free_head = head;
    vq->nfree = (uint16_t)(vq->nfree + n);
    return cookie;
}

unsigned vq_free(const virtqueue_t *vq) { return vq->nfree; }

void vq_disable_cb(virtqueue_t *vq) {
    if (!vq->event_idx) vq->avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
}

bool vq_enable_cb(virtqueue_t *vq) {
    if (vq->event_idx) used_event(vq) = vq->last_used;
    else vq->avail->flags &= (uint16_t)~VRING_AVAIL_F_NO_INTERRUPT;
    mb();                                   /* publish the setting before looking for work that slipped in */
    return vq_pending(vq);
}

void vq_set_priv(virtqueue_t *vq, void *priv) { vq->priv = priv; }
void *vq_priv(const virtqueue_t *vq) { return vq->priv; }
virtio_device_t *vq_device(const virtqueue_t *vq) { return vq->vdev; }
