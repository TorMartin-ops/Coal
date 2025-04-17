#ifndef FS_CONFIG_H
#define FS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * fs_config.h - File System Configuration
 *
 * This header defines configurable parameters and default settings for
 * file system modules.
 */

/* Filesystem type identifiers */
#define FS_TYPE_FAT12       0x01
#define FS_TYPE_FAT16       0x02
#define FS_TYPE_FAT32       0x03
#define FS_TYPE_EXT2        0x04
#define FS_TYPE_VFS         0x10

/* Maximum filename length (modern systems typically allow up to 255 characters) */
#ifndef FS_MAX_FILENAME_LENGTH
#define FS_MAX_FILENAME_LENGTH   255
#endif

/* Maximum full path length */
#ifndef FS_MAX_PATH_LENGTH
#define FS_MAX_PATH_LENGTH       4096
#endif

/* Default block size (bytes) */
#ifndef FS_DEFAULT_BLOCK_SIZE
#define FS_DEFAULT_BLOCK_SIZE    512
#endif

/* Cluster size for FAT file systems (bytes) */
#ifndef FS_FAT_CLUSTER_SIZE
#define FS_FAT_CLUSTER_SIZE      4096
#endif

/* Root directory entry count for FAT12/16 */
#ifndef FS_FAT_ROOT_ENTRY_COUNT
#define FS_FAT_ROOT_ENTRY_COUNT  512
#endif

/* Filesystem cache size (bytes) */
#ifndef FS_CACHE_SIZE
#define FS_CACHE_SIZE            (64 * 1024) // 64 KB
#endif

/* Maximum number of open files per process */
#ifndef FS_MAX_OPEN_FILES
#define FS_MAX_OPEN_FILES        64
#endif

/* Default mount options (bitmask flags) */
#define FS_MOUNT_OPTION_READONLY    0x01
#define FS_MOUNT_OPTION_SYNCHRONOUS 0x02
#define FS_MOUNT_OPTION_NOEXEC      0x04

 #define ROOT_DEVICE_NAME "hdb"
 #define ROOT_FS_TYPE     "FAT"


/**
 * fs_config_t
 *
 * Global configuration parameters for a file system.
 */
typedef struct fs_config {
    uint32_t fs_type;            /* Type of file system (FS_TYPE_*) */
    uint32_t block_size;         /* Block size in bytes */
    uint32_t cluster_size;       /* Cluster size in bytes */
    uint32_t root_entry_count;   /* Number of root directory entries (for FAT) */
    uint32_t cache_size;         /* Filesystem cache size in bytes */
    uint32_t max_open_files;     /* Maximum open files per process */
    uint32_t mount_options;      /* Default mount options (bitmask) */
} fs_config_t;

/* Default configuration for a FAT32 file system. */
static const fs_config_t fs_config_fat_default = {
    .fs_type = FS_TYPE_FAT32,
    .block_size = FS_DEFAULT_BLOCK_SIZE,
    .cluster_size = FS_FAT_CLUSTER_SIZE,
    .root_entry_count = FS_FAT_ROOT_ENTRY_COUNT,
    .cache_size = FS_CACHE_SIZE,
    .max_open_files = FS_MAX_OPEN_FILES,
    .mount_options = FS_MOUNT_OPTION_SYNCHRONOUS
};

/* Default configuration for an ext2 file system. */
static const fs_config_t fs_config_ext2_default = {
    .fs_type = FS_TYPE_EXT2,
    .block_size = 1024,
    .cluster_size = 1024,       /* ext2 uses block allocation */
    .root_entry_count = 0,      /* Not applicable for ext2 */
    .cache_size = FS_CACHE_SIZE,
    .max_open_files = FS_MAX_OPEN_FILES,
    .mount_options = 0
};

#ifdef __cplusplus
}
#endif

#endif /* FS_CONFIG_H */
