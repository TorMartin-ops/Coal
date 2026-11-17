/**
 * @file virtio_console.c
 * @brief virtio-console as /dev/hvc0: a byte stream in both directions, and optionally the kernel log.
 *
 * Single port mode (no MULTIPORT): queue 0 receives, queue 1 transmits. Received bytes are copied
 * into a ring that read() drains; when the ring is full the current buffer is held back, which
 * stops the host from sending until the reader catches up. read() blocks, write() blocks while
 * every transmit buffer is with the device. "console=hvc0" on the command line sends kernel log
 * lines through the emergency-write register.
 */
#define LOG_MODULE "virtio-console"

#include <kernel/drivers/virtio/virtio.h>
#include <kernel/drivers/wait.h>
#include <kernel/core/cmdline.h>
#include <kernel/core/log.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

#define F_EMERG_WRITE 2
#define CFG_EMERG_WR  8

#define MAX_CONSOLES 2
#define BUF_SIZE     4096
#define RX_BUFS      8
#define TX_BUFS      8
#define RING_SIZE    32768u             /* power of two */
#define HVC_MAJOR    229

typedef struct {
    virtio_device_t *vdev;
    virtqueue_t *rxq, *txq;
    uint8_t *ring;
    uint32_t head, tail;                /* free running */
    uint8_t *held;                      /* a received buffer that did not fit in the ring yet */
    uint32_t held_len, held_off;
    uint8_t *tx_free[TX_BUFS];
    unsigned ntx_free;
    drv_waitq_t rx_wait, tx_wait;
    devfs_char_ops_t ops;
    char name[8];
} hvc_t;

static hvc_t *g_cons[MAX_CONSOLES];
static unsigned g_ncons;

/* Moves received data into the ring and hands drained buffers back to the device. IRQs off. */
static void rx_pump(hvc_t *c) {
    for (;;) {
        if (!c->held) {
            c->held = vq_get_buf(c->rxq, &c->held_len);
            c->held_off = 0;
            if (!c->held) break;
            c->held_len = MIN(c->held_len, (uint32_t)BUF_SIZE);
        }
        uint32_t room = RING_SIZE - (c->head - c->tail), n = MIN(room, c->held_len - c->held_off);
        for (uint32_t i = 0; i < n; i++) c->ring[c->head++ & (RING_SIZE - 1)] = c->held[c->held_off + i];
        c->held_off += n;
        if (c->held_off < c->held_len) break;                   /* ring full: keep the rest until read() drains it */
        vq_add(c->rxq, &(vq_sg_t){ c->held, BUF_SIZE }, 0, 1, c->held);
        c->held = NULL;
    }
    vq_kick(c->rxq);
}

static void rx_done(virtqueue_t *vq) {
    hvc_t *c = vq_priv(vq);
    rx_pump(c);
    if (c->head != c->tail) drv_wake_all(&c->rx_wait);
}

static void tx_done(virtqueue_t *vq) {
    hvc_t *c = vq_priv(vq);
    uint8_t *b;

    while ((b = vq_get_buf(vq, NULL))) c->tx_free[c->ntx_free++] = b;
    drv_wake_all(&c->tx_wait);
}

static ssize_t hvc_read(void *priv, void *buf, size_t len) {
    hvc_t *c = priv;
    uintptr_t fl = local_irq_save();
    ssize_t rc = 0;

    while (c->head == c->tail && !(rc = drv_wait(&c->rx_wait))) { }
    if (rc == 0) {
        size_t n = MIN(len, (size_t)(c->head - c->tail));
        for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = c->ring[c->tail++ & (RING_SIZE - 1)];
        rc = (ssize_t)n;
        rx_pump(c);                                            /* room was made: continue with a held buffer */
    }
    local_irq_restore(fl);
    return rc;
}

static ssize_t hvc_write(void *priv, const void *buf, size_t len) {
    hvc_t *c = priv;
    uintptr_t fl = local_irq_save();
    size_t done = 0;
    int rc = 0;

    while (done < len && !rc) {
        while (done < len && c->ntx_free) {                    /* queue as much as there are buffers, then notify once */
            uint8_t *b = c->tx_free[--c->ntx_free];
            size_t n = MIN(len - done, (size_t)BUF_SIZE);
            memcpy(b, (const uint8_t *)buf + done, n);
            vq_add(c->txq, &(vq_sg_t){ b, (uint32_t)n }, 1, 0, b);
            done += n;
        }
        vq_kick(c->txq);
        if (done < len) rc = drv_wait(&c->tx_wait);
    }
    local_irq_restore(fl);
    return done ? (ssize_t)done : rc;
}

static uint32_t hvc_poll(void *priv) {
    hvc_t *c = priv;
    uintptr_t fl = local_irq_save();
    uint32_t ev = (c->head != c->tail ? POLLIN : 0) | (c->ntx_free ? POLLOUT : 0);
    local_irq_restore(fl);
    return ev;
}

/* Kernel log through the emergency-write register: one byte per device access, so only for log lines. */
static hvc_t *g_log_console;

static void log_sink(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        virtio_write_config(g_log_console->vdev, CFG_EMERG_WR, &ch, 1);
    }
}

static bool console_requested(const char *name) {
    char word[16];
    for (unsigned i = 0; cmdline_get("console", i, word, sizeof(word)); i++)
        if (strcmp(word, name) == 0) return true;
    return false;
}

static int console_probe(virtio_device_t *d) {
    if (g_ncons == MAX_CONSOLES) return -1;
    hvc_t *c = kmalloc(sizeof(*c));
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->vdev = d;
    c->ring = kmalloc(RING_SIZE);
    c->rxq = virtio_queue_create(d, 0, RX_BUFS, rx_done);
    c->txq = virtio_queue_create(d, 1, TX_BUFS, tx_done);
    if (!c->ring || !c->rxq || !c->txq) return -1;
    vq_set_priv(c->rxq, c);
    vq_set_priv(c->txq, c);
    for (unsigned i = 0; i < TX_BUFS; i++) {
        uint8_t *b = kmalloc(BUF_SIZE);
        if (!b) return -1;
        c->tx_free[c->ntx_free++] = b;
    }
    for (unsigned i = 0; i < RX_BUFS; i++) {
        uint8_t *b = kmalloc(BUF_SIZE);
        if (!b) return -1;
        vq_add(c->rxq, &(vq_sg_t){ b, BUF_SIZE }, 0, 1, b);
    }

    strcpy(c->name, "hvc0");
    c->name[3] = (char)('0' + g_ncons);
    c->ops = (devfs_char_ops_t){ hvc_read, hvc_write, NULL, c, (HVC_MAJOR << 8) | g_ncons, hvc_poll };
    if (devfs_register_char(c->name, &c->ops) != 0) return -1;
    g_cons[g_ncons++] = c;
    d->priv = c;
    virtio_device_ready(d);
    vq_kick(c->rxq);

    if (virtio_has(d, F_EMERG_WRITE) && !g_log_console && console_requested(c->name)) {
        g_log_console = c;
        klog_set_sink(log_sink);
    }
    LOG_INFO("/dev/%s%s", c->name, g_log_console == c ? ", kernel log" : "");
    return 0;
}

const virtio_driver_t virtio_console_driver = {
    .name = "virtio-console",
    .id = VIRTIO_ID_CONSOLE,
    .features = VIRTIO_FEATURE(F_EMERG_WRITE),
    .probe = console_probe,
};
