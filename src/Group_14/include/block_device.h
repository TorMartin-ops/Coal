#pragma once
#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "types.h"

/* block_device_t represents a low‑level block device (e.g., ATA disk). */
typedef struct {
    const char *device_name;
    uint16_t io_base;
    uint16_t control_base;
    uint32_t sector_size;
    uint32_t total_sectors;
} block_device_t;

/* Initializes the block device.
 * 'device' is a device identifier (e.g., "ata0").
 * Fills in the provided block_device_t structure.
 * Returns 0 on success, negative error code on failure.
 */
int block_device_init(const char *device, block_device_t *dev);

/* Reads 'count' sectors from the block device starting at LBA into buffer.
 * Returns 0 on success.
 */
int block_device_read(block_device_t *dev, uint32_t lba, void *buffer, size_t count);

/* Writes 'count' sectors from buffer to the block device starting at LBA.
 * Returns 0 on success.
 */
int block_device_write(block_device_t *dev, uint32_t lba, const void *buffer, size_t count);

#endif /* BLOCK_DEVICE_H */
