/**
 * @file virtio_rng.c
 * @brief virtio-rng: host entropy for the kernel generator, once at boot and then every few seconds.
 */
#define LOG_MODULE "virtio-rng"

#include <kernel/drivers/virtio/virtio.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/core/log.h>
#include <kernel/lib/random.h>
#include <kernel/memory/kmalloc.h>

#define REQUEST_BYTES 64
#define RESEED_MS     10000

static struct {
    virtqueue_t *vq;
    uint8_t *buf;                       /* heap memory: kernel statics are not direct mapped */
    struct ktimer timer;
} g;

static void request(void) {
    vq_add(g.vq, &(vq_sg_t){ g.buf, REQUEST_BYTES }, 0, 1, g.buf);
    vq_kick(g.vq);
}

static void reseed(struct ktimer *t) {
    (void)t;
    request();
}

static void rng_done(virtqueue_t *vq) {
    uint32_t len;

    while (vq_get_buf(vq, &len)) {
        random_add(g.buf, len);
        timer_add(&g.timer, RESEED_MS);
    }
}

static int rng_probe(virtio_device_t *d) {
    if (g.vq) return -1;
    g.buf = kmalloc(REQUEST_BYTES);
    g.vq = g.buf ? virtio_queue_create(d, 0, 1, rng_done) : NULL;
    if (!g.vq) return -1;
    g.timer.fn = reseed;
    virtio_device_ready(d);
    request();
    return 0;
}

const virtio_driver_t virtio_rng_driver = {
    .name = "virtio-rng",
    .id = VIRTIO_ID_RNG,
    .probe = rng_probe,
};
