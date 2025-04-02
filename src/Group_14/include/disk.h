#pragma once
#ifndef DISK_H
#define DISK_H

#include "types.h"

/* disk_t represents a high‑level disk device. */
typedef struct {
    const char *device_name;   // e.g., "ata0"
    uint16_t io_base;          // I/O port base for the device
    uint16_t control_base;     // Control port base
    uint32_t sector_size;      // In bytes (typically 512)
    uint32_t total_sectors;    // Total number of sectors on the disk
} disk_t;

/* Initialize a disk given a device identifier (e.g., "ata0").
 * Returns 0 on success, or a negative error code.
 */
int disk_init(disk_t *disk, const char *device);

/* Read a single sector from the disk at the given LBA into buffer.
 * Returns 0 on success.
 */
int disk_read_sector(disk_t *disk, uint32_t lba, void *buffer);

/* Write a single sector from buffer to the disk at the given LBA.
 * Returns 0 on success.
 */
int disk_write_sector(disk_t *disk, uint32_t lba, const void *buffer);

/* Read multiple sectors starting at LBA.
 * Returns 0 on success.
 */
int disk_read_sectors(disk_t *disk, uint32_t lba, void *buffer, size_t count);

/* Write multiple sectors starting at LBA.
 * Returns 0 on success.
 */
int disk_write_sectors(disk_t *disk, uint32_t lba, const void *buffer, size_t count);

#endif /* DISK_H */
