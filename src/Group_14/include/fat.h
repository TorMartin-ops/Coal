#pragma once
#ifndef FAT_H
#define FAT_H

#include "types.h"
#include "disk.h" // Include disk.h for disk_t type
#include "vfs.h"  // Include vfs.h for vfs_driver_t (if needed directly)

#ifdef __cplusplus
extern "C" {
#endif

/* FAT filesystem types */
#define FAT_TYPE_FAT12  12
#define FAT_TYPE_FAT16  16
#define FAT_TYPE_FAT32  32

/* For FAT32, define the End-Of-Chain marker. */
#define FAT32_EOC 0x0FFFFFFF

/* BIOS Parameter Block (BPB) & Boot Sector structure */
#pragma pack(push, 1)
typedef struct {
    uint8_t  jump_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_short; /* if zero, use total_sectors_long */
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;
    /* FAT32 Extended BPB fields */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t boot_sector_signature;
} fat_boot_sector_t;
#pragma pack(pop)

/* Directory entry for FAT */
#pragma pack(push, 1)
typedef struct {
    uint8_t  name[11];     // 8.3 filename format
    uint8_t  attr;         // File attributes
    uint8_t  nt_reserved;  // Reserved for use by Windows NT
    uint8_t  creation_time_tenth; // Tenths of a second timestamp for creation time
    uint16_t creation_time; // Time file was created
    uint16_t creation_date; // Date file was created
    uint16_t last_access_date; // Last access date
    uint16_t first_cluster_high; // High word of this entry's first cluster number (FAT32)
    uint16_t write_time;    // Time of last write
    uint16_t write_date;    // Date of last write
    uint16_t first_cluster_low; // Low word of this entry's first cluster number
    uint32_t file_size;     // 32-bit file size in bytes
} fat_dir_entry_t;
#pragma pack(pop)

/* FAT filesystem instance structure */
typedef struct fat_fs {
    disk_t disk;                   // Underlying disk device structure
    fat_boot_sector_t boot_sector; // Copy of boot sector data
    uint32_t fat_size;             // FAT size in sectors
    uint32_t total_sectors;        // Total sectors on the volume
    uint32_t first_data_sector;    // First data sector (after reserved+FAT+root dir)
    uint32_t root_dir_sectors;     // Number of sectors for root directory (FAT12/16 only)
    uint32_t cluster_count;        // Number of data clusters
    uint8_t  type;                 // FAT type: FAT12, FAT16, or FAT32
    void *fat_table;               // Pointer to the in-memory FAT table
    // Add mutex/lock here if supporting concurrency
} fat_fs_t;

/*
 * NOTE: The primary way to interact with the FAT filesystem should now be
 * through the VFS functions (vfs_open, vfs_read, etc.) after registering
 * the FAT driver and mounting a FAT volume.
 * The fat_file_t structure is removed as file state is managed internally
 * via the VFS file_t and the driver's internal context (fat_file_context_t).
 */

/* FAT driver registration with VFS */
int fat_register_driver(void);
void fat_unregister_driver(void);

/* fat_readdir remains as a potentially useful helper, though not part of the core VFS driver API */
/* Reads directory entries from a given path (currently only root supported and needs rework) */
int fat_readdir(fat_fs_t *fs, const char *path, fat_dir_entry_t **entries, size_t *entry_count);

#ifdef __cplusplus
}
#endif

#endif /* FAT_H */