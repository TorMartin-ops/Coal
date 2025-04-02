#pragma once
#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "types.h"

/**
 * @brief Structure representing a low-level ATA PIO block device.
 */
typedef struct {
    const char *device_name;   // Identifier (e.g., "hda", "hdb")
    uint16_t io_base;          // Base I/O port address (e.g., 0x1F0)
    uint16_t control_base;     // Control port address (e.g., 0x3F6)
    bool is_slave;             // Flag: true if device is Slave, false if Master
    uint32_t sector_size;      // Size of a sector in bytes (typically 512)
    uint32_t total_sectors;    // Total number of sectors discovered via IDENTIFY
} block_device_t;

// Function declarations remain the same
int block_device_init(const char *device, block_device_t *dev);
int block_device_read(block_device_t *dev, uint32_t lba, void *buffer, size_t count);
int block_device_write(block_device_t *dev, uint32_t lba, const void *buffer, size_t count);

#endif /* BLOCK_DEVICE_H */