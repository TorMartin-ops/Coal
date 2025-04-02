#include "disk.h"
#include "block_device.h"   // Lower-level block device interface
#include "terminal.h"
#include "kmalloc.h"
#include <string.h>
#include "types.h"

/*
 * disk_probe:
 * Probe the underlying block device and fill in a block_device_t structure.
 * Returns 0 on success.
 */
static int disk_probe(block_device_t *bd, const char *device) {
    if (block_device_init(device, bd) != 0) {
        terminal_write("[Disk] Probe: Failed to initialize block device.\n");
        return -1;
    }
    return 0;
}

/*
 * disk_init:
 * Initializes the disk structure by probing the underlying block device.
 */
int disk_init(disk_t *disk, const char *device) {
    if (!disk || !device) {
        terminal_write("[Disk] disk_init: Invalid parameters.\n");
        return -1;
    }

    block_device_t bd;
    if (disk_probe(&bd, device) != 0) {
        terminal_write("[Disk] disk_init: Device probe failed.\n");
        return -1;
    }

    /* Populate our disk structure from the block device info. */
    disk->device_name = bd.device_name;
    disk->io_base = bd.io_base;
    disk->control_base = bd.control_base;
    disk->sector_size = bd.sector_size;
    disk->total_sectors = bd.total_sectors;

    terminal_write("[Disk] Initialized disk: ");
    terminal_write(device);
    terminal_write("\n");
    return 0;
}

int disk_read_sector(disk_t *disk, uint32_t lba, void *buffer) {
    if (!disk || !buffer) {
        terminal_write("[Disk] disk_read_sector: Invalid parameters.\n");
        return -1;
    }
    int ret = block_device_read((block_device_t *)disk, lba, buffer, 1);
    if (ret != 0) {
        terminal_write("[Disk] disk_read_sector: Read operation failed.\n");
    }
    return ret;
}

int disk_write_sector(disk_t *disk, uint32_t lba, const void *buffer) {
    if (!disk || !buffer) {
        terminal_write("[Disk] disk_write_sector: Invalid parameters.\n");
        return -1;
    }
    int ret = block_device_write((block_device_t *)disk, lba, buffer, 1);
    if (ret != 0) {
        terminal_write("[Disk] disk_write_sector: Write operation failed.\n");
    }
    return ret;
}

int disk_read_sectors(disk_t *disk, uint32_t lba, void *buffer, size_t count) {
    if (!disk || !buffer || count == 0) {
        terminal_write("[Disk] disk_read_sectors: Invalid parameters.\n");
        return -1;
    }
    int ret = block_device_read((block_device_t *)disk, lba, buffer, count);
    if (ret != 0) {
        terminal_write("[Disk] disk_read_sectors: Multi-sector read failed.\n");
    }
    return ret;
}

int disk_write_sectors(disk_t *disk, uint32_t lba, const void *buffer, size_t count) {
    if (!disk || !buffer || count == 0) {
        terminal_write("[Disk] disk_write_sectors: Invalid parameters.\n");
        return -1;
    }
    int ret = block_device_write((block_device_t *)disk, lba, buffer, count);
    if (ret != 0) {
        terminal_write("[Disk] disk_write_sectors: Multi-sector write failed.\n");
    }
    return ret;
}
