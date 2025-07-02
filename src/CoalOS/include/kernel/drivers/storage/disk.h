#ifndef DISK_H
#define DISK_H

#include <kernel/drivers/storage/block_device.h> // Includes types like uint32_t, size_t, bool
#include <kernel/fs/vfs/fs_errno.h>     // For error codes like FS_SUCCESS

// --- Configuration ---
#define MAX_PARTITIONS_PER_DISK 4 // Standard MBR limit for primary partitions

// --- Structures ---

/**
 * @brief Represents a partition on a disk.
 */
typedef struct partition {
    struct disk *parent_disk; // Pointer back to the disk this partition belongs to
    uint8_t     partition_index; // Index (0-3 for MBR primary)
    bool        is_valid;       // Whether this partition entry is valid/parsed
    uint8_t     type;           // Partition type code (e.g., 0x83 for Linux, 0x07 for NTFS/exFAT, 0x0C for FAT32 LBA)
    uint64_t    start_lba;      // Starting LBA sector of the partition (absolute on disk)
    uint64_t    total_sectors;  // Size of the partition in sectors
    // Add other relevant info if needed (e.g., bootable flag)
} partition_t;

/**
 * @brief Represents a logical disk, potentially containing partitions.
 * Wraps the underlying block device.
 */
typedef struct disk {
    block_device_t blk_dev;         // The underlying block device (e.g., ATA drive)
    bool           initialized;     // Has this disk structure been initialized?
    bool           has_mbr;         // Was a valid MBR signature found?
    partition_t    partitions[MAX_PARTITIONS_PER_DISK]; // Parsed MBR partitions
    // Add other disk-wide info if needed (e.g., disk GUID for GPT)
} disk_t;


// --- Function Prototypes ---

/**
 * @brief Initializes a disk structure, probing the underlying block device and parsing partitions.
 * @param disk Pointer to the disk_t structure to initialize.
 * @param device_name The kernel name for the block device (e.g., "hda", "hdb").
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_init(disk_t *disk, const char *device_name);

/**
 * @brief Reads sectors directly from the underlying block device (ignores partitions).
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (absolute on disk).
 * @param buffer Pointer to the buffer where data will be stored.
 * @param count Number of sectors to read.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_read_raw_sectors(disk_t *disk, uint64_t lba, void *buffer, size_t count);

/**
 * @brief Writes sectors directly to the underlying block device (ignores partitions).
 * @param disk Pointer to the initialized disk_t structure.
 * @param lba The starting Logical Block Address (absolute on disk).
 * @param buffer Pointer to the buffer containing data to write.
 * @param count Number of sectors to write.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int disk_write_raw_sectors(disk_t *disk, uint64_t lba, const void *buffer, size_t count);


/**
 * @brief Reads sectors from a specific partition.
 * @param partition Pointer to the initialized partition_t structure.
 * @param lba The starting Logical Block Address *relative to the start of the partition*.
 * @param buffer Pointer to the buffer where data will be stored.
 * @param count Number of sectors to read.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int partition_read_sectors(partition_t *partition, uint64_t lba, void *buffer, size_t count);

/**
 * @brief Writes sectors to a specific partition.
 * @param partition Pointer to the initialized partition_t structure.
 * @param lba The starting Logical Block Address *relative to the start of the partition*.
 * @param buffer Pointer to the buffer containing data to write.
 * @param count Number of sectors to write.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
int partition_write_sectors(partition_t *partition, uint64_t lba, const void *buffer, size_t count);


/**
 * @brief Gets a pointer to a partition structure by its index.
 * @param disk Pointer to the initialized disk_t structure.
 * @param index The partition index (0-3 for MBR primary).
 * @return Pointer to the partition_t structure, or NULL if index is invalid or partition is not valid.
 */
partition_t* disk_get_partition(disk_t *disk, uint8_t index);

/**
 * @brief Gets the total number of sectors for the entire disk.
 * @param disk Pointer to the initialized disk_t structure.
 * @return Total sectors, or 0 if disk is invalid.
 */
uint64_t disk_get_total_sectors(disk_t *disk);

/**
 * @brief Simplified disk read for ATA driver (from disk_stub.c)
 * @param lba Starting LBA
 * @param count Number of sectors to read
 * @param buffer Buffer to read into
 * @return 0 on success, negative on error
 */
int disk_read_sectors(uint64_t lba, size_t count, void* buffer);

/**
 * @brief Simplified disk write for ATA driver (from disk_stub.c)
 * @param lba Starting LBA
 * @param count Number of sectors to write
 * @param buffer Buffer to write from
 * @return 0 on success, negative on error
 */
int disk_write_sectors(uint64_t lba, size_t count, const void* buffer);

/**
 * @brief Set the system disk instance
 * @param disk Disk instance to use as system disk
 */
void disk_set_system_disk(disk_t* disk);

#endif // DISK_H