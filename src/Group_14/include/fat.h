#pragma once
#ifndef FAT_H
#define FAT_H

#include "types.h"
#include "disk.h" // Include disk.h for disk_t type
#include "vfs.h"  // Include vfs.h for vfs_driver_t (needed for registration funcs)
#include "spinlock.h" 

#ifdef __cplusplus
extern "C" {
#endif

/* FAT filesystem types */
#define FAT_TYPE_FAT12  1 // Define locally if not globally available
#define FAT_TYPE_FAT16  2
#define FAT_TYPE_FAT32  3

/* Standardized End-Of-Chain marker representation used internally */
// (The actual EOC marker on disk varies by FAT type,
// but the driver can convert to this standard value when reading)
#define FAT_EOC_MARKER 0x0FFFFFFF // Use a value suitable for all FAT types (FAT32's range)

/* FAT Directory Entry Attributes */
#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20
#define ATTR_LONG_NAME      (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)
#define ATTR_LONG_NAME_MASK (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_DIRECTORY | ATTR_ARCHIVE)

/* Special first byte markers for directory entries */
#define DIR_ENTRY_DELETED   0xE5 // Entry is deleted
#define DIR_ENTRY_UNUSED    0x00 // Entry is unused and all subsequent entries are unused

/* LFN Entry definitions */
#define LFN_ENTRY_LAST      0x40 // Mask for sequence number byte indicates last LFN entry
#define MAX_LFN_ENTRIES     20   // Maximum number of LFN entries for one file name


// --- On-Disk Structures ---
// Ensure consistent packing for on-disk structures

#pragma pack(push, 1)

/**
 * @brief BIOS Parameter Block (BPB) and Boot Sector structure.
 * Covers fields for FAT12/16 and the FAT32 extensions.
 */
typedef struct {
    uint8_t  jump_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;     // Max root entries for FAT12/16
    uint16_t total_sectors_short;  // Use total_sectors_long if this is 0
    uint8_t  media;
    uint16_t fat_size_16;          // Sectors per FAT for FAT12/16
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;   // Use if total_sectors_short is 0

    // FAT32 Extended BPB fields start here (offset 36)
    uint32_t fat_size_32;          // Sectors per FAT for FAT32
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;         // Starting cluster of root dir for FAT32
    uint16_t fs_info;              // Sector number of FSINFO struct (usually 1)
    uint16_t backup_boot_sector;   // Sector number of backup boot sector
    uint8_t  reserved[12];         // Reserved for future expansion
    uint8_t  drive_number;         // BIOS drive number (e.g., 0x80)
    uint8_t  reserved1;
    uint8_t  boot_signature;       // Extended boot signature (0x29)
    uint32_t volume_id;            // Volume serial number
    uint8_t  volume_label[11];     // Volume label (padded with spaces)
    uint8_t  fs_type[8];           // Filesystem type string (e.g., "FAT32   ")

    // The rest is boot code, size varies
    // uint8_t  boot_code[...];
    // uint16_t boot_sector_signature; // 0xAA55 (at offset 510)

} fat_boot_sector_t;


/**
 * @brief FAT Standard 8.3 Directory Entry structure (32 bytes).
 */
typedef struct {
    uint8_t  name[11];             // 8.3 filename (padded with spaces)
    uint8_t  attr;                 // File attributes (ATTR_* flags)
    uint8_t  nt_reserved;          // Reserved for use by Windows NT
    uint8_t  creation_time_tenth;  // Tenths of a second timestamp (0-199)
    uint16_t creation_time;        // Time file was created
    uint16_t creation_date;        // Date file was created
    uint16_t last_access_date;     // Last access date
    uint16_t first_cluster_high;   // High word of first cluster number (FAT32)
    uint16_t write_time;           // Time of last write
    uint16_t write_date;           // Date of last write
    uint16_t first_cluster_low;    // Low word of first cluster number
    uint32_t file_size;            // 32-bit file size in bytes

} fat_dir_entry_t;


/**
 * @brief FAT Long File Name (LFN) Directory Entry structure (32 bytes).
 */
typedef struct {
    uint8_t   seq_num;             // Sequence number (ORed with LFN_ENTRY_LAST if last)
    uint16_t  name1[5];            // Characters 1-5 (UTF-16)
    uint8_t   attr;                // Attributes (Must be ATTR_LONG_NAME)
    uint8_t   type;                // Type (Should be 0 for LFN)
    uint8_t   checksum;            // Checksum of 8.3 name
    uint16_t  name2[6];            // Characters 6-11 (UTF-16)
    uint16_t  first_cluster;       // Must be 0 for LFN entries
    uint16_t  name3[2];            // Characters 12-13 (UTF-16)

} fat_lfn_entry_t;

#pragma pack(pop) // Restore default packing

// --- In-Memory Filesystem Structure ---

/**
 * @brief In-memory representation of a mounted FAT filesystem instance.
 */
typedef struct fat_fs {
    /* Core Disk Information */
    disk_t *disk_ptr;              // Pointer to underlying disk device structure
    
    /* BPB Derived Values */
    uint32_t fat_size;             // Size (in sectors) of ONE FAT
    uint32_t total_sectors;        // Total sectors on the volume
    uint32_t first_data_sector;    // LBA of the first data sector (cluster 2)
    uint32_t root_dir_sectors;     // Number of sectors for root directory (FAT12/16 only)
    uint32_t cluster_count;        // Total number of data clusters
    uint8_t  type;                 // FAT type: FAT_TYPE_FAT12, FAT_TYPE_FAT16, or FAT_TYPE_FAT32
    uint8_t  num_fats;             // Number of FAT copies on the disk
    
    /* In-Memory FAT Table */
    void *fat_table;               // Pointer to the in-memory FAT table (size = fat_size * bytes_per_sector)

    /* Cached geometry info derived from boot sector */
    uint32_t root_cluster;         // Starting cluster of root dir (FAT32 only)
    uint32_t sectors_per_cluster;  // Number of sectors per cluster
    uint32_t bytes_per_sector;     // Number of bytes per sector
    uint32_t cluster_size_bytes;   // bytes_per_sector * sectors_per_cluster
    uint32_t fat_start_lba;        // LBA where the first FAT starts
    uint32_t root_dir_start_lba;   // LBA where the fixed root dir starts (FAT12/16 only)
    
    /* Standardized End-of-Chain marker value used by the driver internally */
    uint32_t eoc_marker;

    /* Concurrency Control */
    spinlock_t lock;

} fat_fs_t;


// --- Public Driver Functions ---

/**
 * @brief Registers the FAT filesystem driver with the VFS.
 * @return 0 on success, negative error code on failure.
 */
int fat_register_driver(void);

/**
 * @brief Unregisters the FAT filesystem driver from the VFS.
 */
void fat_unregister_driver(void);


/*
 * NOTE: Specific file operation functions (mount, open, read, etc.)
 * are typically declared as static within fat.c and assigned to the
 * vfs_driver_t structure there. They are not part of the public API
 * defined in this header, except for the registration functions.
 */

#ifdef __cplusplus
}
#endif

#endif /* FAT_H */