#include "disk.h"
#include "block_device.h"   // For block_device_init, block_device_read/write
#include "terminal.h"       // For logging
#include "types.h"
#include <string.h>         // For memset

/**
 * disk_init:
 * Initializes the disk_t structure by initializing the underlying block device.
 */
int disk_init(disk_t *disk, const char *device_name) {
    if (!disk || !device_name) {
        terminal_write("[Disk] disk_init: Invalid parameters.\n");
        return -1; // Invalid parameters
    }

    // Zero initialize the disk structure first
    memset(disk, 0, sizeof(disk_t));

    // Initialize the embedded block_device_t structure.
    // block_device_init will perform the ATA IDENTIFY and fill geometry info.
    int ret = block_device_init(device_name, &disk->blk_dev);
    if (ret != 0) {
        terminal_write("[Disk] disk_init: Underlying block device initialization failed for ");
        terminal_write(device_name);
        terminal_write(".\n");
        disk->initialized = false;
        return -1; // Initialization failed
    }

    // Disk is now considered initialized
    disk->initialized = true;
    terminal_write("[Disk] Initialized logical disk: ");
    terminal_write(disk->blk_dev.device_name); // Use name from block_device
    terminal_write("\n");
    return 0; // Success
}

/**
 * disk_read_sectors:
 * Reads one or more sectors by calling the block device layer.
 */
int disk_read_sectors(disk_t *disk, uint32_t lba, void *buffer, size_t count) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        terminal_write("[Disk] disk_read_sectors: Invalid parameters or disk not initialized.\n");
        return -1;
    }

    // Delegate directly to the block device function
    int ret = block_device_read(&disk->blk_dev, lba, buffer, count);
    if (ret != 0) {
        terminal_write("[Disk] disk_read_sectors: block_device_read failed.\n");
        // Error logged in block_device_read already
    }
    return ret;
}

/**
 * disk_write_sectors:
 * Writes one or more sectors by calling the block device layer.
 */
int disk_write_sectors(disk_t *disk, uint32_t lba, const void *buffer, size_t count) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        terminal_write("[Disk] disk_write_sectors: Invalid parameters or disk not initialized.\n");
        return -1;
    }

    // Delegate directly to the block device function
    int ret = block_device_write(&disk->blk_dev, lba, buffer, count);
    if (ret != 0) {
         terminal_write("[Disk] disk_write_sectors: block_device_write failed.\n");
         // Error logged in block_device_write already
    }
    return ret;
}