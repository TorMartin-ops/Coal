/**
 * @file disk.h
 * @brief Logical disk: a block device plus its MBR partition table.
 *
 * All functions return BLOCK_ERR_* codes (0 on success).
 */
#ifndef DISK_H
#define DISK_H

#include <kernel/drivers/storage/block_device.h>

#define MAX_PARTITIONS_PER_DISK 4 /* MBR primary partitions */

typedef struct partition {
    bool     is_valid;
    uint8_t  type;
    uint64_t start_lba;     /* absolute sector on the disk */
    uint64_t total_sectors;
} partition_t;

typedef struct disk {
    block_device_t blk_dev;
    bool           initialized;
    bool           has_mbr;
    partition_t    partitions[MAX_PARTITIONS_PER_DISK];
} disk_t;

/** Probes the named drive (e.g. "hda") and reads its partition table. */
int disk_init(disk_t *disk, const char *device_name);

/** Whole-disk sector access (absolute LBA, partitions are ignored). */
int disk_read_raw_sectors(disk_t *disk, uint64_t lba, void *buffer, size_t count);
int disk_write_raw_sectors(disk_t *disk, uint64_t lba, const void *buffer, size_t count);

/** Scatter/gather variant: one command for the sectors of all segments (see block_device_rw). */
int disk_rw_sg(disk_t *disk, uint64_t lba, const blk_seg_t *segs, unsigned nseg, bool write);

/** Flushes the drive's write cache. */
int disk_flush(disk_t *disk);

#endif /* DISK_H */
