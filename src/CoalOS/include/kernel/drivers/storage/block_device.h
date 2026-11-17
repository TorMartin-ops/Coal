/**
 * @file block_device.h
 * @brief Block devices: an operations table over sector-addressed storage, and the ATA driver
 *        (primary and secondary channel, LBA28/LBA48, bus-master DMA or PIO).
 *
 * ATA transfers are polled with interrupts enabled; the drive's own interrupt is
 * disabled (nIEN), so there is no completion IRQ to wait for. virtio-blk (see
 * virtio_blk.h) plugs in through the same operations.
 */
#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <kernel/core/types.h>
#include <kernel/cpu/isr_frame.h>

#define BLOCK_ERR_OK           0
#define BLOCK_ERR_PARAMS      -1
#define BLOCK_ERR_TIMEOUT     -2
#define BLOCK_ERR_DEV_ERR     -3 /* ERR bit set */
#define BLOCK_ERR_DEV_FAULT   -4 /* DF bit set */
#define BLOCK_ERR_NO_DEV      -5
#define BLOCK_ERR_BOUNDS      -6
#define BLOCK_ERR_UNSUPPORTED -7
#define BLOCK_ERR_IO          -10

/** A run of sectors in memory; a transfer is a list of these, contiguous on the disk. */
typedef struct {
    void    *buf;
    uint32_t sectors;
} blk_seg_t;

typedef struct block_device block_device_t;

/** What a driver provides; the checks in block_device_rw() have run when these are called. */
typedef struct blk_ops {
    int (*rw)(block_device_t *dev, uint64_t lba, const blk_seg_t *segs, unsigned nseg, bool write);
    int (*flush)(block_device_t *dev);
} blk_ops_t;

struct block_device {
    const blk_ops_t *ops;
    void       *priv;          /* driver private */
    const char *device_name;   /* "hda".."hdd", "vda".. */
    uint16_t io_base;
    uint16_t control_base;
    uint8_t  channel;          /* 0 = primary, 1 = secondary */
    bool     is_slave;
    bool     initialized;
    bool     lba48_supported;
    uint32_t sector_size;
    uint64_t total_sectors;
};

/** Probes the named drive with IDENTIFY and fills in dev. */
int block_device_init(const char *device, block_device_t *dev);

int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count);
int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count);

/** One transfer of consecutive sectors from/to several memory segments (kernel direct-mapped memory). */
int block_device_rw(block_device_t *dev, uint64_t lba, const blk_seg_t *segs, unsigned nseg, bool write);

/** Flushes the drive's write cache. */
int block_device_flush(block_device_t *dev);

/** Safety net: acknowledges a stray IRQ14 (the drive's interrupt is disabled). */
void ata_primary_irq_handler(isr_frame_t *frame);

#endif /* BLOCK_DEVICE_H */
