#pragma once
#ifndef FAT_H
#define FAT_H


#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FAT filesystem types */
#define FAT_TYPE_FAT12  12
#define FAT_TYPE_FAT16  16
#define FAT_TYPE_FAT32  32

/* For FAT32, define the End-Of-Chain marker.
 * (For FAT12/16 this value is different.)
 */
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
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_time_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;  // For FAT32
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} fat_dir_entry_t;
#pragma pack(pop)

/* FAT filesystem instance */
typedef struct {
    const char *device;            // Device identifier (e.g., "hd0")
    fat_boot_sector_t boot_sector; // Copy of boot sector data
    uint32_t fat_size;             // FAT size in sectors
    uint32_t total_sectors;        // Total sectors on the volume
    uint32_t first_data_sector;    // First data sector (after reserved+FAT+root dir)
    uint32_t root_dir_sectors;     // Number of sectors for root directory (FAT12/16)
    uint32_t cluster_count;        // Number of data clusters
    uint8_t  type;                 // FAT type: FAT12, FAT16, or FAT32
    void *fat_table;               // Pointer to the in‑memory FAT table (array of uint32_t for FAT32)
} fat_fs_t;

/* FAT file handle */
typedef struct {
    fat_fs_t *fs;            // Associated filesystem
    uint32_t first_cluster;  // First cluster of the file
    uint32_t current_cluster;// Current cluster in file chain
    uint32_t file_size;      // Size of the file in bytes
    uint32_t pos;            // Current byte offset in the file
} fat_file_t;

/* FAT driver integration with VFS */
typedef struct vfs_driver vfs_driver_t;  // Forward declaration (defined in vfs.h)

/* FAT Filesystem API */
int fat_register_driver(void);
void fat_unregister_driver(void);

int fat_mount(const char *device, fat_fs_t *fs);
int fat_unmount(fat_fs_t *fs);
int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file);
int fat_read(fat_fs_t *fs, fat_file_t *file, void *buf, size_t len, size_t *read_bytes);
int fat_write(fat_fs_t *fs, fat_file_t *file, const void *buf, size_t len, size_t *written_bytes);
int fat_close(fat_fs_t *fs, fat_file_t *file);
int fat_readdir(fat_fs_t *fs, const char *path, fat_dir_entry_t **entries, size_t *entry_count);

#ifdef __cplusplus
}
#endif

#endif /* FAT_H */
