#include "disk.h"
#include "block_device.h"   // Underlying device operations
#include "terminal.h"       // Logging
#include "fs_errno.h"       // Error codes (includes the new FS_ERR_OUT_OF_BOUNDS)
#include "kmalloc.h"        // For temporary buffer allocation
#include "types.h"          // Standard types
#include "assert.h"         // For KERNEL_ASSERT

#include <string.h>         // For memset, memcpy

// --- MBR Structure Definitions ---
// Standard MBR Partition Entry (16 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  boot_indicator; // 0x80 = bootable, 0x00 = non-bootable
    uint8_t  start_head;
    uint16_t start_sector_cylinder; // Encoded start sector/cylinder (CHS) - Ignore for LBA
    uint8_t  partition_type;
    uint8_t  end_head;
    uint16_t end_sector_cylinder;   // Encoded end sector/cylinder (CHS) - Ignore for LBA
    uint32_t start_lba;      // Starting LBA sector of the partition
    uint32_t total_sectors;  // Number of sectors in the partition
} mbr_partition_entry_t;

// Standard MBR Structure (relevant parts)
typedef struct __attribute__((packed)) {
    uint8_t               bootstrap_code[446]; // Boot code area
    mbr_partition_entry_t partitions[4];     // Partition table (4 entries)
    uint16_t              signature;         // MBR signature (0xAA55)
} master_boot_record_t;

// Ensure MBR structure size is correct (should be 512)
// Can uncomment after adding assert.h include
// KERNEL_ASSERT(sizeof(master_boot_record_t) == 512, "MBR struct size mismatch");


// --- Static Helper Prototypes ---
static int parse_mbr(disk_t *disk);


/**
 * @brief Initializes a disk structure, probing the underlying block device and parsing partitions.
 * Initializes the block device, reads the MBR, parses partitions, and marks the disk as ready.
 * @param disk Pointer to the disk_t structure to initialize.
 * @param device_name The kernel name for the block device (e.g., "hda", "hdb").
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_init(disk_t *disk, const char *device_name) {
    if (!disk || !device_name) {
        terminal_printf("[Disk] disk_init: Error - Invalid parameters (disk=%p, name=%s).\n", disk, device_name ? device_name : "NULL");
        return FS_ERR_INVALID_PARAM; // Use enum value directly
    }

    // Zero initialize the entire disk structure
    memset(disk, 0, sizeof(disk_t));

    // 1. Initialize the underlying block device
    // This probes the device (e.g., ATA IDENTIFY) and gets its geometry/name.
    terminal_printf("[Disk] Initializing block device '%s'...\n", device_name);
    int ret = block_device_init(device_name, &disk->blk_dev);
    if (ret != FS_SUCCESS) {
        terminal_printf("[Disk] disk_init: Error - Underlying block device init failed for '%s' (code %d).\n",
                       device_name, ret);
        disk->initialized = false; // Ensure it's marked as not initialized
        return ret; // Propagate block device error
    }
     // Use the name confirmed by the block device layer
    terminal_printf("[Disk] Block device '%s' initialized. Total Sectors: %llu\n",
                    disk->blk_dev.device_name, disk->blk_dev.total_sectors);


    // 2. Attempt to parse the MBR partition table
    ret = parse_mbr(disk);
    if (ret != FS_SUCCESS) {
        // Don't treat MBR parsing failure as a fatal disk init error.
        // The disk might be unpartitioned or use a different scheme (like GPT).
        // We can still use the raw disk access functions.
        terminal_printf("[Disk] disk_init: Warning - Failed to parse MBR on '%s' (code %d). Disk usable for raw access.\n",
                       disk->blk_dev.device_name, ret);
        disk->has_mbr = false;
        // Continue initialization, partitions will just be marked invalid.
    } else {
        disk->has_mbr = true;
        terminal_printf("[Disk] disk_init: Successfully parsed MBR on '%s'.\n", disk->blk_dev.device_name);
    }

    // 3. Mark disk as initialized (even if MBR parsing failed)
    disk->initialized = true;
    terminal_printf("[Disk] Logical disk '%s' initialization complete.\n", disk->blk_dev.device_name);
    return FS_SUCCESS;
}

/**
 * @brief Reads sectors directly from the underlying block device (ignores partitions).
 * Provides raw access to the disk, bypassing partition logic.
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (absolute on disk).
 * @param buffer Pointer to the buffer where data will be stored.
 * @param count Number of sectors to read.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_read_raw_sectors(disk_t *disk, uint64_t lba, void *buffer, size_t count) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        terminal_printf("[Disk] read_raw: Error - Invalid parameters (disk=%p, init=%d, buf=%p, count=%u).\n",
                       disk, disk ? disk->initialized : 0, buffer, count);
        return FS_ERR_INVALID_PARAM; // Use enum value directly
    }

    // Bounds check against the whole disk size
    if (lba >= disk->blk_dev.total_sectors || count > disk->blk_dev.total_sectors - lba) {
         terminal_printf("[Disk] read_raw: Error - Read out of bounds (LBA=%llu, Count=%u, Total=%llu).\n",
                        lba, count, disk->blk_dev.total_sectors);
         // Corrected: Use the defined error code
         return FS_ERR_OUT_OF_BOUNDS;
    }

    // Delegate directly to the block device function
    int ret = block_device_read(&disk->blk_dev, lba, buffer, count);
    if (ret != FS_SUCCESS) {
        // Error already logged by block_device layer
        terminal_printf("[Disk] read_raw: Block device read failed for %u sectors at LBA %llu.\n", count, lba);
    }
    return ret;
}

/**
 * @brief Writes sectors directly to the underlying block device (ignores partitions).
 * Provides raw access to the disk, bypassing partition logic.
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (absolute on disk).
 * @param buffer Pointer to the buffer containing data to write.
 * @param count Number of sectors to write.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_write_raw_sectors(disk_t *disk, uint64_t lba, const void *buffer, size_t count) {
    if (!disk || !disk->initialized || !buffer || count == 0) {
        terminal_printf("[Disk] write_raw: Error - Invalid parameters (disk=%p, init=%d, buf=%p, count=%u).\n",
                       disk, disk ? disk->initialized : 0, buffer, count);
        return FS_ERR_INVALID_PARAM; // Use enum value directly
    }

    // Bounds check against the whole disk size
    if (lba >= disk->blk_dev.total_sectors || count > disk->blk_dev.total_sectors - lba) {
         terminal_printf("[Disk] write_raw: Error - Write out of bounds (LBA=%llu, Count=%u, Total=%llu).\n",
                        lba, count, disk->blk_dev.total_sectors);
         // Corrected: Use the defined error code
         return FS_ERR_OUT_OF_BOUNDS;
    }

    // Delegate directly to the block device function
    int ret = block_device_write(&disk->blk_dev, lba, buffer, count);
     if (ret != FS_SUCCESS) {
        // Error already logged by block_device layer
        terminal_printf("[Disk] write_raw: Block device write failed for %u sectors at LBA %llu.\n", count, lba);
    }
    return ret;
}


/**
 * @brief Reads sectors from a specific partition.
 * Translates the partition-relative LBA to an absolute disk LBA and performs bounds checks.
 * @param partition Pointer to the initialized partition_t structure.
 * @param lba The starting Logical Block Address *relative to the start of the partition*.
 * @param buffer Pointer to the buffer where data will be stored.
 * @param count Number of sectors to read.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int partition_read_sectors(partition_t *partition, uint64_t lba, void *buffer, size_t count) {
     if (!partition || !partition->is_valid || !partition->parent_disk || !buffer || count == 0) {
          terminal_printf("[Disk] part_read: Error - Invalid parameters (part=%p, valid=%d, disk=%p, buf=%p, count=%u).\n",
                         partition, partition ? partition->is_valid : 0, partition ? partition->parent_disk : NULL, buffer, count);
         return FS_ERR_INVALID_PARAM; // Use enum value directly
     }

     // Bounds check against the partition size
     if (lba >= partition->total_sectors || count > partition->total_sectors - lba) {
         terminal_printf("[Disk] part_read: Error - Read out of partition bounds (Part LBA=%llu, Count=%u, Part Size=%llu).\n",
                        lba, count, partition->total_sectors);
         // Corrected: Use the defined error code
         return FS_ERR_OUT_OF_BOUNDS;
     }

     // Translate partition LBA to absolute disk LBA
     uint64_t absolute_lba = partition->start_lba + lba;

     // Delegate to the raw disk read function (which uses the block device)
     // Note: disk_read_raw_sectors performs its own bounds check against the whole disk,
     // which is implicitly correct if the partition info is valid.
     return disk_read_raw_sectors(partition->parent_disk, absolute_lba, buffer, count);
}

/**
 * @brief Writes sectors to a specific partition.
 * Translates the partition-relative LBA to an absolute disk LBA and performs bounds checks.
 * @param partition Pointer to the initialized partition_t structure.
 * @param lba The starting Logical Block Address *relative to the start of the partition*.
 * @param buffer Pointer to the buffer containing data to write.
 * @param count Number of sectors to write.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int partition_write_sectors(partition_t *partition, uint64_t lba, const void *buffer, size_t count) {
     if (!partition || !partition->is_valid || !partition->parent_disk || !buffer || count == 0) {
          terminal_printf("[Disk] part_write: Error - Invalid parameters (part=%p, valid=%d, disk=%p, buf=%p, count=%u).\n",
                         partition, partition ? partition->is_valid : 0, partition ? partition->parent_disk : NULL, buffer, count);
         return FS_ERR_INVALID_PARAM; // Use enum value directly
     }

     // Bounds check against the partition size
     if (lba >= partition->total_sectors || count > partition->total_sectors - lba) {
         terminal_printf("[Disk] part_write: Error - Write out of partition bounds (Part LBA=%llu, Count=%u, Part Size=%llu).\n",
                        lba, count, partition->total_sectors);
         // Corrected: Use the defined error code
         return FS_ERR_OUT_OF_BOUNDS;
     }

     // Translate partition LBA to absolute disk LBA
     uint64_t absolute_lba = partition->start_lba + lba;

     // Delegate to the raw disk write function
     return disk_write_raw_sectors(partition->parent_disk, absolute_lba, buffer, count);
}

/**
 * @brief Gets a pointer to a partition structure by its index.
 * @param disk Pointer to the initialized disk_t structure.
 * @param index The partition index (0-3 for MBR primary).
 * @return Pointer to the partition_t structure if valid, or NULL otherwise.
 */
partition_t* disk_get_partition(disk_t *disk, uint8_t index) {
    if (!disk || !disk->initialized || index >= MAX_PARTITIONS_PER_DISK) {
        return NULL;
    }
    // Return pointer only if the partition was marked as valid during parsing
    return disk->partitions[index].is_valid ? &disk->partitions[index] : NULL;
}

/**
 * @brief Gets the total number of sectors for the entire disk.
 * @param disk Pointer to the initialized disk_t structure.
 * @return Total sectors, or 0 if disk is invalid/uninitialized.
 */
uint64_t disk_get_total_sectors(disk_t *disk) {
     if (!disk || !disk->initialized) {
         return 0;
     }
     return disk->blk_dev.total_sectors;
}


// --- Static Helper Implementations ---

/**
 * @brief Reads and parses the Master Boot Record (MBR) of a disk.
 * Populates the partition information within the disk_t structure.
 * @param disk Pointer to the disk_t structure (block device must be initialized).
 * @return FS_SUCCESS on success (valid MBR found and parsed), negative error code otherwise.
 */
static int parse_mbr(disk_t *disk) {
    // Corrected: Added message string to KERNEL_ASSERT
    KERNEL_ASSERT(disk != NULL && disk->initialized, "Disk pointer must be non-NULL and initialized for MBR parsing");

    // Allocate a temporary buffer for the MBR sector
    // Use kmalloc as sector size might be > stack limits
    uint32_t sector_size = disk->blk_dev.sector_size;
    if (sector_size < 512) { // MBR requires at least 512 bytes
         terminal_printf("[Disk MBR] Error: Disk sector size %u is less than required 512 bytes.\n", sector_size);
         return FS_ERR_INVALID_FORMAT; // Use enum value directly
    }
    uint8_t *mbr_buffer = kmalloc(sector_size);
    if (!mbr_buffer) {
        terminal_printf("[Disk MBR] Error: Failed to allocate buffer for MBR read.\n");
        return FS_ERR_OUT_OF_MEMORY; // Use enum value directly
    }

    // Read the first sector (LBA 0)
    terminal_printf("[Disk MBR] Reading MBR sector (LBA 0) from '%s'...\n", disk->blk_dev.device_name);
    int ret = block_device_read(&disk->blk_dev, 0, mbr_buffer, 1);
    if (ret != FS_SUCCESS) {
        terminal_printf("[Disk MBR] Error: Failed to read MBR sector (LBA 0) from '%s' (code %d).\n",
                       disk->blk_dev.device_name, ret);
        kfree(mbr_buffer);
        return ret; // Propagate read error
    }

    // Check MBR signature (0xAA55 at offset 510)
    master_boot_record_t *mbr = (master_boot_record_t *)mbr_buffer;
    if (mbr->signature != 0xAA55) {
        terminal_printf("[Disk MBR] Warning: Invalid MBR signature (0x%04X) found on '%s'. No partitions parsed.\n",
                       mbr->signature, disk->blk_dev.device_name);
        kfree(mbr_buffer);
        return FS_ERR_INVALID_FORMAT; // Indicate invalid MBR format, use enum value directly
    }

    terminal_printf("[Disk MBR] Valid MBR signature found. Parsing partitions...\n");

    // Parse the 4 primary partition entries
    for (int i = 0; i < MAX_PARTITIONS_PER_DISK; i++) {
        mbr_partition_entry_t *entry = &mbr->partitions[i];
        partition_t *part = &disk->partitions[i];

        part->partition_index = i;
        part->parent_disk = disk; // Link back to parent

        // Check if the partition entry is potentially active (type != 0)
        // and has a non-zero size. Some tools create empty entries with type 0.
        if (entry->partition_type != 0 && entry->total_sectors != 0) {
            part->is_valid = true;
            part->type = entry->partition_type;
            part->start_lba = entry->start_lba;
            part->total_sectors = entry->total_sectors;

            // Basic sanity check: Partition should not start beyond disk boundaries
            // or extend beyond disk boundaries.
            if (part->start_lba >= disk->blk_dev.total_sectors ||
                part->total_sectors > disk->blk_dev.total_sectors - part->start_lba)
            {
                 terminal_printf("[Disk MBR] Warning: Partition %d on '%s' seems invalid (Start=%llu, Size=%llu, DiskSize=%llu). Marking invalid.\n",
                                i, disk->blk_dev.device_name, part->start_lba, part->total_sectors, disk->blk_dev.total_sectors);
                 part->is_valid = false; // Mark as invalid due to bounds issue
            } else {
                 terminal_printf("  [+] Partition %d: Type=0x%02X, StartLBA=%llu, Size=%llu sectors\n",
                                i, part->type, part->start_lba, part->total_sectors);
            }
        } else {
            // Empty or unused partition entry
            part->is_valid = false;
            part->type = 0;
            part->start_lba = 0;
            part->total_sectors = 0;
        }
    }

    kfree(mbr_buffer);
    return FS_SUCCESS; // Successfully parsed (even if some entries were invalid)
}