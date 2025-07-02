/**
 * @file fat_core.h
 * @brief Core structures, constants, and registration for FAT filesystem driver.
 *
 * Defines the fundamental data structures representing the FAT filesystem,
 * file context, boot sector, and directory entries (both 8.3 and LFN).
 * Also declares the primary functions for registering and unregistering the
 * FAT driver with the Virtual File System (VFS).
 */

 #ifndef FAT_CORE_H
 #define FAT_CORE_H
 
 #include <libc/stdint.h> // For standard integer types (uint8_t, uint16_t, uint32_t)
 #include <libc/stdbool.h> // For bool type
 #include <kernel/core/types.h>      // For kernel-specific types like size_t, off_t if not in stdint/def
 #include <kernel/fs/vfs/vfs.h>        // For vfs_driver_t, vnode_t, file_t, struct dirent
 #include <kernel/drivers/storage/disk.h>       // For disk_t definition
 #include <kernel/sync/spinlock.h>   // For spinlock_t
 
 /* --- FAT Type Constants --- */
 #define FAT_TYPE_FAT12 1
 #define FAT_TYPE_FAT16 2
 #define FAT_TYPE_FAT32 3
 
 /* --- FAT Attribute Constants (for fat_dir_entry_t.attr) --- */
 #define FAT_ATTR_READ_ONLY      0x01
 #define FAT_ATTR_HIDDEN         0x02
 #define FAT_ATTR_SYSTEM         0x04
 #define FAT_ATTR_VOLUME_ID      0x08
 #define FAT_ATTR_DIRECTORY      0x10
 #define FAT_ATTR_ARCHIVE        0x20
 #define FAT_ATTR_LONG_NAME      (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)
 #define FAT_ATTR_LONG_NAME_MASK (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID | FAT_ATTR_DIRECTORY | FAT_ATTR_ARCHIVE)
 
 
 /* --- FAT Boot Sector / BIOS Parameter Block (BPB) --- */
 // Structure representing the FAT BPB, common fields first, then FAT32 extensions.
 typedef struct __attribute__((packed)) {
     uint8_t  jump_boot[3];          // Jump instruction to boot code (e.g., 0xEB xx 0x90)
     uint8_t  oem_name[8];           // OEM Name Identifier (e.g., "MSWIN4.1")
     uint16_t bytes_per_sector;      // Size of a sector in bytes (usually 512, 1024, 2048, 4096)
     uint8_t  sectors_per_cluster;   // Number of sectors in an allocation unit (cluster) (must be power of 2)
     uint16_t reserved_sector_count; // Number of sectors before the first FAT (usually 1 for FAT12/16, 32 for FAT32)
     uint8_t  num_fats;              // Number of FAT tables (usually 2)
     uint16_t root_entry_count;      // Max number of entries in root dir (FAT12/16 only, 0 for FAT32)
     uint16_t total_sectors_short;   // Total sectors if < 65536 (0 if >= 65536)
     uint8_t  media_type;            // Media descriptor (e.g., 0xF8 for hard disk)
     uint16_t fat_size_16;           // Sectors per FAT (FAT12/16 only, 0 for FAT32)
     uint16_t sectors_per_track;     // Geometry: Sectors per track
     uint16_t num_heads;             // Geometry: Number of heads
     uint32_t hidden_sectors;        // Count of hidden sectors preceding this partition
     uint32_t total_sectors_long;    // Total sectors (used if total_sectors_short is 0)
 
     // --- FAT32 Extended Fields (EBPB - Extended BIOS Parameter Block) ---
     // These fields start at offset 36 from the beginning of the boot sector.
     // Check fs_type string or fat_size_16=0 to confirm FAT32 before using.
     uint32_t fat_size_32;           // Sectors per FAT (FAT32 only)
     uint16_t ext_flags;             // Extended flags (e.g., mirroring control)
     uint16_t fs_version;            // Filesystem version (major/minor, usually 0:0)
     uint32_t root_cluster;          // First cluster of the root directory (usually 2)
     uint16_t fs_info_sector;        // Sector number of FSINFO struct (usually 1)
     uint16_t backup_boot_sector;    // Sector number of the backup boot sector (usually 6)
     uint8_t  reserved_32[12];       // Reserved for future expansion
     uint8_t  drive_number;          // BIOS drive number (e.g., 0x80 for first hard disk)
     uint8_t  reserved_nt;           // Reserved for Windows NT flags
     uint8_t  boot_signature_ext;    // Extended boot signature (0x29)
     uint32_t volume_id;             // Volume serial number
     uint8_t  volume_label[11];      // Volume label string (padded with spaces)
     uint8_t  fs_type_str[8];        // Filesystem type string (e.g., "FAT32   ")
     // Boot code follows...
     // Boot sector signature 0xAA55 at offset 510
 } fat_boot_sector_t;
 
 /* --- FAT Directory Entry (Short 8.3 Name) --- */
 // Represents a standard 32-byte directory entry.
 typedef struct __attribute__((packed)) {
     uint8_t  name[11];              // Short filename (8 chars) + extension (3 chars), space padded. First byte special meanings (0xE5=deleted, 0x00=unused end, 0x05->0xE5 for real KANJI name start).
     uint8_t  attr;                  // File attributes (see FAT_ATTR_* defines)
     uint8_t  nt_res;                // Reserved for Windows NT (used for case info)
     uint8_t  creation_time_tenth;   // Creation time, tenths of a second (0-199)
     uint16_t creation_time;         // Creation time (H:M:S format)
     uint16_t creation_date;         // Creation date (Y:M:D format)
     uint16_t last_access_date;      // Last access date (Y:M:D format)
     uint16_t first_cluster_high;    // High 16 bits of first cluster number (0 for FAT12/16)
     uint16_t write_time;            // Last modification time
     uint16_t write_date;            // Last modification date
     uint16_t first_cluster_low;     // Low 16 bits of first cluster number
     uint32_t file_size;             // File size in bytes (0 for directories)
 } fat_dir_entry_t;
 
 /* --- FAT Long Filename (LFN) Directory Entry --- */
 // Represents a 32-byte LFN entry. Multiple LFN entries precede the 8.3 entry.
 typedef struct __attribute__((packed)) {
     uint8_t  seq_num;               // Sequence number (ORed with 0x40 for last entry)
     uint16_t name1[5];              // First 5 UTF-16 characters (Little Endian)
     uint8_t  attr;                  // Attributes (always FAT_ATTR_LONG_NAME)
     uint8_t  type;                  // Type (always 0 for LFN)
     uint8_t  checksum;              // Checksum of the short filename
     uint16_t name2[6];              // Next 6 UTF-16 characters
     uint16_t first_cluster_zero;    // Must be zero for LFN entries
     uint16_t name3[2];              // Last 2 UTF-16 characters
 } fat_lfn_entry_t;
 
 /* --- FAT Filesystem Instance Structure --- */
 // Holds all runtime state for a mounted FAT filesystem.
 typedef struct {
     // Disk and Locking
     disk_t    *disk_ptr;            // Pointer to the underlying disk device structure
     spinlock_t lock;                // Spinlock to protect concurrent access to this structure and FAT table
 
     // Filesystem Geometry & Type (parsed from Boot Sector)
     uint8_t    type;                // FAT type (FAT_TYPE_FAT12, FAT_TYPE_FAT16, FAT_TYPE_FAT32)
     uint16_t   bytes_per_sector;    // Sector size in bytes
     uint8_t    sectors_per_cluster; // Cluster size in sectors
     uint32_t   cluster_size_bytes;  // Cluster size in bytes (bytes_per_sector * sectors_per_cluster)
     uint32_t   total_sectors;       // Total sectors on the partition
     uint32_t   fat_size_sectors;    // Size of one FAT table in sectors (use fat_size_16 or fat_size_32)
     uint8_t    num_fats;            // Number of FAT copies (usually 2)
     uint32_t   fat_start_lba;       // Starting LBA (sector) of the first FAT table
     uint32_t   root_dir_sectors;    // Number of sectors occupied by the root directory (FAT12/16 only)
     uint32_t   root_dir_start_lba;  // Starting LBA of the root directory (FAT12/16 only)
     uint32_t   first_data_sector;   // Starting LBA of the data region (cluster area)
     uint32_t   total_data_clusters; // Total number of data clusters available
     uint32_t   root_cluster;        // Cluster number of the root directory (FAT32 only, usually 2)
     uint32_t   eoc_marker;          // End-of-chain marker value for this FAT type (e.g., 0xFF8, 0xFFF8, 0x0FFFFFF8)
     // FSINFO related fields (optional, could be added later for performance)
     // uint32_t fs_info_sector;     // Sector number of FSINFO
     // uint32_t free_cluster_count; // Cached count of free clusters
     // uint32_t next_free_cluster;  // Hint for next free cluster search
 
     // In-Memory FAT Table Cache
     void      *fat_table;           // Pointer to the cached FAT table in memory
     size_t     fat_table_size_bytes;// Size of the allocated fat_table buffer
     bool       fat_dirty;           // Flag indicating if the in-memory FAT needs flushing
 
 } fat_fs_t;
 
 /* --- FAT File/Directory Context Structure --- */
 // Holds runtime state for an opened file or directory within a FAT filesystem.
 // This structure is typically stored in file->vnode->data.
 typedef struct {
     fat_fs_t *fs;                   // Pointer back to the filesystem instance this belongs to
 
     // File/Directory Identification & Metadata
     uint32_t first_cluster;         // First cluster number of the file/directory's data
     uint32_t file_size;             // Current size of the file in bytes (from directory entry)
     uint32_t dir_entry_cluster;     // Cluster number where the directory entry for this file/dir resides
     uint32_t dir_entry_offset;      // Byte offset within dir_entry_cluster of the 8.3 directory entry
     bool     is_directory;          // True if this context represents a directory
 
     // State Flags
     bool     dirty;                 // True if metadata (size, first cluster) changed and needs update on close
 
     // Sequential I/O State (optimization, could be removed if lseek recalculates)
     // uint32_t current_cluster;    // Last cluster accessed for sequential read/write
     // uint32_t offset_in_cluster;  // Offset within the current_cluster
 
     // Readdir State (only relevant if is_directory is true)
     uint32_t readdir_current_cluster; // Cluster being scanned for readdir
     uint32_t readdir_current_offset;  // Byte offset within the directory data being scanned
     size_t   readdir_last_index;      // The logical index of the last entry returned by readdir
 
 } fat_file_context_t;
 
 
 /* --- Public Driver Functions --- */
 
 /**
  * @brief Registers the FAT filesystem driver with the VFS.
  * Must be called during kernel initialization after VFS is ready.
  * @return 0 on success, negative error code on failure.
  */
 int fat_register_driver(void);
 
 /**
  * @brief Unregisters the FAT filesystem driver from the VFS.
  * Should be called during shutdown if necessary.
  */
 void fat_unregister_driver(void);
 
 /**
  * @brief Helper function to extract the full cluster number from a directory entry.
  * Combines the high and low word fields.
  * @param e Pointer to the FAT directory entry.
  * @return The 32-bit cluster number (0 if entry is NULL or cluster fields are 0).
  */
 uint32_t fat_get_entry_cluster(const fat_dir_entry_t *e);
 
 #endif /* FAT_CORE_H */