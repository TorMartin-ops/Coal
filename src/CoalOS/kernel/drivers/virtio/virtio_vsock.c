/**
 * @file virtio_vsock.c
 * @brief virtio-vsock device: receive and transmit queues carrying packets for kernel/net/vsock.c.
 *
 * Every receive buffer is one 4 KiB block (header plus payload). Transmit uses a fixed set of
 * 8 KiB slots. While earlier packets are still with the device, consecutive data packets of one
 * connection are merged into the open slot and go out together when the device reports progress.
 */
#define LOG_MODULE "virtio-vsock"

#include <kernel/drivers/virtio/virtio.h>
#include <kernel/drivers/virtio/virtio_vsock.h>
#include <kernel/net/vsock.h>
#include <kernel/core/log.h>
#include <libc/errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define RX_BUFS      32
#define RX_BUF_SIZE  4096
#define TX_SLOTS     16
#define TX_SLOT_SIZE 8192               /* header + VSOCK_MAX_PAYLOAD */
#define EVENT_RESET  0                  /* VIRTIO_VSOCK_EVENT_TRANSPORT_RESET */

static struct {
    virtqueue_t *rx, *tx, *ev;
    uint64_t cid;
    uint8_t *free_slot[TX_SLOTS];
    unsigned nfree, inflight;
    uint8_t *open;                      /* slot being filled with data for one connection, not yet submitted */
    uint32_t open_len;
    uint32_t *event;                    /* event queue entry; heap memory, kernel statics are not direct mapped */
} g;

uint64_t virtio_vsock_cid(void) { return g.cid; }

static void flush_open(void) {
    if (!g.open) return;
    vq_sg_t sg = { g.open, VSOCK_HDR_SIZE + g.open_len };
    vq_add(g.tx, &sg, 1, 0, g.open);        /* the queue has a descriptor for every slot */
    vq_kick(g.tx);
    g.inflight++;
    g.open = NULL;
}

int virtio_vsock_send(const struct vsock_hdr *hdr, const void *payload, uint32_t len) {
    struct vsock_hdr *oh = (struct vsock_hdr *)g.open;
    bool data = hdr->op == VSOCK_OP_RW && len;

    if (g.open && data && oh->src_port == hdr->src_port && oh->dst_port == hdr->dst_port &&
        oh->dst_cid == hdr->dst_cid && g.open_len + len <= VSOCK_MAX_PAYLOAD) {
        memcpy(g.open + VSOCK_HDR_SIZE + g.open_len, payload, len);
        g.open_len += len;
        oh->len = g.open_len;
        oh->fwd_cnt = hdr->fwd_cnt;         /* the newest credit information goes out */
        oh->buf_alloc = hdr->buf_alloc;
    } else {
        flush_open();                       /* keep packets in order */
        if (!g.nfree) return -ENOBUFS;
        uint8_t *slot = g.free_slot[--g.nfree];
        memcpy(slot, hdr, VSOCK_HDR_SIZE);
        if (len) memcpy(slot + VSOCK_HDR_SIZE, payload, len);
        ((struct vsock_hdr *)slot)->len = len;
        g.open = slot;
        g.open_len = len;
        if (!data) flush_open();
    }
    if (g.open && (!g.inflight || g.open_len == VSOCK_MAX_PAYLOAD)) flush_open();
    return 0;
}

static void tx_done(virtqueue_t *vq) {
    uint8_t *slot;

    while ((slot = vq_get_buf(vq, NULL))) {
        g.free_slot[g.nfree++] = slot;
        g.inflight--;
    }
    flush_open();                           /* the device caught up: send what was merged meanwhile */
    vsock_tx_ready();
}

static void rx_post(uint8_t *buf) {
    vq_sg_t sg = { buf, RX_BUF_SIZE };
    vq_add(g.rx, &sg, 0, 1, buf);
}

static void rx_done(virtqueue_t *vq) {
    uint8_t *buf;
    uint32_t len;

    while ((buf = vq_get_buf(vq, &len))) {
        const struct vsock_hdr *h = (const struct vsock_hdr *)buf;
        if (len >= VSOCK_HDR_SIZE && h->len <= len - VSOCK_HDR_SIZE) vsock_rx(h, buf + VSOCK_HDR_SIZE);
        rx_post(buf);
    }
    vq_kick(vq);
}

static void event_done(virtqueue_t *vq) {
    while (vq_get_buf(vq, NULL)) {
        if (*g.event == EVENT_RESET) vsock_transport_reset();
        vq_add(vq, &(vq_sg_t){ g.event, sizeof(*g.event) }, 0, 1, g.event);
    }
    vq_kick(vq);
}

static int vsock_probe(virtio_device_t *d) {
    if (g.cid) return -1;                   /* one device */
    virtio_read_config(d, 0, &g.cid, sizeof(g.cid));
    g.rx = virtio_queue_create(d, 0, RX_BUFS, rx_done);
    g.tx = virtio_queue_create(d, 1, TX_SLOTS, tx_done);
    g.ev = virtio_queue_create(d, 2, 1, event_done);
    if (!g.cid || !g.rx || !g.tx || !g.ev) { g.cid = 0; return -1; }

    g.event = kmalloc(sizeof(*g.event));
    if (!g.event) { g.cid = 0; return -1; }
    for (unsigned i = 0; i < TX_SLOTS; i++) {
        g.free_slot[i] = kmalloc(TX_SLOT_SIZE);
        if (!g.free_slot[i]) { g.cid = 0; return -1; }
    }
    g.nfree = TX_SLOTS;
    for (unsigned i = 0; i < RX_BUFS; i++) {
        uint8_t *buf = kmalloc(RX_BUF_SIZE);
        if (!buf) break;
        rx_post(buf);
    }
    vq_add(g.ev, &(vq_sg_t){ g.event, sizeof(*g.event) }, 0, 1, g.event);
    virtio_device_ready(d);
    vq_kick(g.rx);
    vq_kick(g.ev);
    LOG_INFO("guest cid %u", (unsigned)g.cid);
    return 0;
}

const virtio_driver_t virtio_vsock_driver = {
    .name = "virtio-vsock",
    .id = VIRTIO_ID_VSOCK,
    .probe = vsock_probe,
};
