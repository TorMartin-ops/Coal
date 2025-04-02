#pragma once
#ifndef DISK_H
#define DISK_H

#include "types.h"
#include "block_device.h" // Include block_device.h for block_device_t

/**
 * @brief Structure representing a logical disk, built upon a block device.
 *
 * This structure holds information about the disk geometry and state,
 * potentially managing partitions or higher-level features in the future.
 * It contains an underlying block_device_t structure for low-level I/O.
 */
typedef struct {
    block_device_t blk_dev;    // Underlying block device structure
    // Add other disk-level metadata if needed (e.g., partition table)
    bool initialized;          // Flag indicating if disk_init was successful
} disk_t;

/**
 * @brief Initializes a disk structure by probing the underlying block device.
 *
 * Calls block_device_init to identify the device and populate the internal
 * block_device_t structure within the disk_t.
 *
 * @param disk Pointer to the disk_t structure to initialize.
 * @param device_name Identifier for the underlying block device (e.g., "ata0").
 * @return 0 on success, or a negative error code on failure.
 */
int disk_init(disk_t *disk, const char *device_name);

/**
 * @brief Reads multiple sectors from the disk.
 *
 * This function acts as a wrapper around block_device_read.
 *
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (LBA).
 * @param buffer Pointer to the buffer where data will be stored.
 * @param count The number of sectors to read.
 * @return 0 on success, negative error code on failure.
 */
int disk_read_sectors(disk_t *disk, uint32_t lba, void *buffer, size_t count);

/**
 * @brief Writes multiple sectors to the disk.
 *
 * This function acts as a wrapper around block_device_write.
 *
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (LBA).
 * @param buffer Pointer to the buffer containing data to write.
 * @param count The number of sectors to write.
 * @return 0 on success, negative error code on failure.
 */
int disk_write_sectors(disk_t *disk, uint32_t lba, const void *buffer, size_t count);

#endif /* DISK_H */