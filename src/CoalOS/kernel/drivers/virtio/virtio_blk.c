/**
 * @file virtio_blk.c
 * @brief virtio-blk: one request queue, scatter/gather requests, flush.
 *
 * Requests complete from the queue interrupt. A caller sleeps until its own request is done;
 * several tasks may have requests in flight at once.
 */
#define LOG_MODULE "virtio-blk"

#include <kernel/drivers/virtio/virtio.h>
#include <kernel/drivers/virtio/virtio_blk.h>
#include <kernel/drivers/wait.h>
#include <kernel/core/log.h>
#include <kernel/memory/dma.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

/* Feature bits */
#define F_SIZE_MAX  1
#define F_SEG_MAX   2
#define F_RO        5
#define F_BLK_SIZE  6
#define F_FLUSH     9

/* Device configuration */
#define CFG_CAPACITY 0
#define CFG_SIZE_MAX 8
#define CFG_SEG_MAX  12
#define CFG_BLK_SIZE 20

/* Request types and status values */
#define T_IN    0
#define T_OUT   1
#define T_FLUSH 4
#define S_OK    0

#define MAX_DISKS     4
#define SECTOR_BYTES  512u
#define MAX_DATA_SEGS 62u               /* descriptors per request besides header and status */

struct blk_request {
    struct { uint32_t type, reserved; uint64_t sector; } hdr;
    uint8_t  status;
    volatile bool done;
};

typedef struct {
    virtio_device_t *vdev;
    virtqueue_t *vq;
    drv_waitq_t wq;                     /* completions and freed descriptors */
    uint64_t capacity;                  /* in 512-byte units */
    uint32_t size_max, seg_max, blk_size;
    bool     read_only, flush;
} virtio_blk_t;

static virtio_blk_t *g_disks[MAX_DISKS];

static void blk_done(virtqueue_t *vq) {
    virtio_blk_t *b = vq_priv(vq);
    struct blk_request *r;

    while ((r = vq_get_buf(vq, NULL))) r->done = true;
    drv_wake_all(&b->wq);
}

/* Issues one request (header + data + status) and sleeps until the device is done with it. */
static int submit(virtio_blk_t *b, uint32_t type, uint64_t sector, const vq_sg_t *data, unsigned nd) {
    struct blk_request *req = kmalloc(sizeof(*req));     /* kernel stacks are not direct mapped: no DMA from them */
    vq_sg_t sg[MAX_DATA_SEGS + 2];
    unsigned n = 0;

    if (!req) return BLOCK_ERR_IO;
    *req = (struct blk_request){ .hdr = { type, 0, sector }, .status = 0xFF };
    sg[n++] = (vq_sg_t){ &req->hdr, sizeof(req->hdr) };
    for (unsigned i = 0; i < nd; i++) sg[n++] = data[i];
    sg[n++] = (vq_sg_t){ &req->status, 1 };

    uintptr_t fl = local_irq_save();
    while (vq_add(b->vq, sg, type == T_OUT ? n - 1 : 1, type == T_OUT ? 1 : n - 1, req) != 0)
        drv_wait_uninterruptible(&b->wq);                   /* ring full: wait for a completion */
    vq_kick(b->vq);
    while (!req->done) drv_wait_uninterruptible(&b->wq);    /* the device owns req until it completes: no early exit */
    local_irq_restore(fl);

    int r = req->status == S_OK ? BLOCK_ERR_OK : req->status == 2 ? BLOCK_ERR_UNSUPPORTED : BLOCK_ERR_IO;
    kfree(req);
    return r;
}

static int blk_rw(block_device_t *dev, uint64_t lba, const blk_seg_t *segs, unsigned nseg, bool write) {
    virtio_blk_t *b = dev->priv;
    uint32_t max_data = b->seg_max ? MIN(b->seg_max, MAX_DATA_SEGS) : MAX_DATA_SEGS;
    uint32_t type = write ? T_OUT : T_IN;
    vq_sg_t data[MAX_DATA_SEGS];
    unsigned nd = 0;
    uint64_t sector = lba * (dev->sector_size / SECTOR_BYTES);      /* where the next descriptor starts */
    uint64_t start = sector;                                         /* first sector of the request being built */

    if (write && b->read_only) return BLOCK_ERR_UNSUPPORTED;
    for (unsigned i = 0; i < nseg; i++)
        if (!dma_reachable(segs[i].buf, (size_t)segs[i].sectors * dev->sector_size)) return BLOCK_ERR_PARAMS;

    for (unsigned i = 0; i < nseg; i++) {
        const uint8_t *p = segs[i].buf;
        uint64_t left = (uint64_t)segs[i].sectors * dev->sector_size;
        while (left) {                                      /* split what the device cannot take in one descriptor */
            uint32_t n = (uint32_t)MIN(left, b->size_max ? b->size_max & ~(SECTOR_BYTES - 1) : (uint64_t)1 << 30);
            if (nd == max_data) {
                int r = submit(b, type, start, data, nd);
                if (r != BLOCK_ERR_OK) return r;
                start = sector;
                nd = 0;
            }
            data[nd++] = (vq_sg_t){ p, n };
            sector += n / SECTOR_BYTES;
            p += n;
            left -= n;
        }
    }
    return nd ? submit(b, type, start, data, nd) : BLOCK_ERR_OK;
}

static int blk_flush(block_device_t *dev) {
    virtio_blk_t *b = dev->priv;
    return b->flush ? submit(b, T_FLUSH, 0, NULL, 0) : BLOCK_ERR_OK;
}

static const blk_ops_t virtio_blk_ops = { blk_rw, blk_flush };

int virtio_blk_attach(const char *name, block_device_t *dev) {
    unsigned idx = (unsigned)(name[2] - 'a');

    if (strlen(name) != 3 || name[0] != 'v' || name[1] != 'd' || idx >= MAX_DISKS) return BLOCK_ERR_PARAMS;
    virtio_blk_t *b = g_disks[idx];
    if (!b) return BLOCK_ERR_NO_DEV;

    memset(dev, 0, sizeof(*dev));
    dev->ops = &virtio_blk_ops;
    dev->priv = b;
    dev->device_name = name;
    dev->sector_size = b->blk_size;
    dev->total_sectors = b->capacity >> __builtin_ctz(b->blk_size / SECTOR_BYTES);
    dev->initialized = dev->total_sectors != 0;
    return dev->initialized ? BLOCK_ERR_OK : BLOCK_ERR_NO_DEV;
}

static int blk_probe(virtio_device_t *d) {
    unsigned idx = 0;
    while (idx < MAX_DISKS && g_disks[idx]) idx++;
    if (idx == MAX_DISKS) return -1;

    virtio_blk_t *b = kmalloc(sizeof(*b));
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    b->vdev = d;
    virtio_read_config(d, CFG_CAPACITY, &b->capacity, sizeof(b->capacity));
    if (virtio_has(d, F_SIZE_MAX)) virtio_read_config(d, CFG_SIZE_MAX, &b->size_max, sizeof(b->size_max));
    if (virtio_has(d, F_SEG_MAX)) virtio_read_config(d, CFG_SEG_MAX, &b->seg_max, sizeof(b->seg_max));
    b->blk_size = SECTOR_BYTES;
    if (virtio_has(d, F_BLK_SIZE)) virtio_read_config(d, CFG_BLK_SIZE, &b->blk_size, sizeof(b->blk_size));
    if (b->blk_size < SECTOR_BYTES || b->blk_size > 8192 || (b->blk_size & (b->blk_size - 1))) b->blk_size = SECTOR_BYTES;
    if (b->size_max && b->size_max < SECTOR_BYTES) { kfree(b); return -1; }
    b->read_only = virtio_has(d, F_RO);
    b->flush = virtio_has(d, F_FLUSH);

    b->vq = virtio_queue_create(d, 0, 128, blk_done);
    if (!b->vq) { kfree(b); return -1; }
    vq_set_priv(b->vq, b);
    g_disks[idx] = b;
    d->priv = b;
    virtio_device_ready(d);
    LOG_INFO("vd%c: %u MiB%s", 'a' + idx, (unsigned)(b->capacity / 2048), b->read_only ? ", read-only" : "");
    return 0;
}

const virtio_driver_t virtio_blk_driver = {
    .name = "virtio-blk",
    .id = VIRTIO_ID_BLOCK,
    .features = VIRTIO_FEATURE(F_SIZE_MAX) | VIRTIO_FEATURE(F_SEG_MAX) | VIRTIO_FEATURE(F_RO) |
                VIRTIO_FEATURE(F_BLK_SIZE) | VIRTIO_FEATURE(F_FLUSH),
    .probe = blk_probe,
};
