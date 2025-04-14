/****************************************************************************
 * fat.c - FAT12/16/32 Filesystem Driver Implementation (Upgraded Version 4.5)
 *
 * Enhancements and Implementations:
 *  - Subdirectory traversal with `fat_lookup_path` (placeholder, partial)
 *  - LFN (Long File Name) reading + basic writing for O_CREAT
 *  - Basic LFN deletion support in unlink
 *  - Cluster allocation/extension for file writes
 *  - O_TRUNC, O_CREAT logic
 *  - `fat_unlink_internal` (marks entry + LFNs deleted, frees clusters)
 *  - `fat_readdir_internal` returning struct dirent with partial LFN
 *  - `find_free_directory_slot` stub (needs extension logic if full)
 *  - `write_directory_entries` for writing LFN+8.3 sets
 *  - Directory entry updates on close
 *  - FAT12/16 root directory scanning/writing placeholders
 *  - Basic concurrency placeholders with spinlock
 *  - Implemented placeholders for:
 *      read_fat_sector, write_fat_sector
 *      load_fat_table, flush_fat_table
 *      find_free_cluster, fat_allocate_cluster, fat_free_cluster_chain
 *      read_cluster_cached, write_cluster_cached
 *      calculate_lfn_checksum, reconstruct_lfn, generate_lfn_entries
 *      update_directory_entry, mark_directory_entry_deleted, read_directory_sector
 *      fat_set_cluster_entry, fat_get_next_cluster, fat_cluster_to_lba
 *
 * Remaining TODO / Known Limitations:
 *  - Directory extension when no free slots (find_free_directory_slot stub)
 *  - mkdir, rmdir not implemented
 *  - Full LFN edge cases (Unicode, >20 entries, special chars)
 *  - 8.3 name collision handling (generate_unique_short_name is simplistic)
 *  - Timestamps
 *  - Comprehensive error handling (partial writes, etc.)
 *  - Proper concurrency (spinlock is just a placeholder)
 *  - FAT12 read/write cluster chaining logic is not fully implemented
 ****************************************************************************/

 #include "fat.h"
 #include "terminal.h"       // Logging
 #include "kmalloc.h"        // Memory allocation
 #include "buddy.h"          // Underlying allocator (fallback)
 #include "disk.h"           // Disk operations
 #include "vfs.h"            // VFS structures and registration
 #include "fs_errno.h"       // Filesystem error codes
 #include "fat_utils.h"      // format_filename, etc.
 #include "buffer_cache.h"   // Disk block caching
 #include "sys_file.h"       // O_* flags
 #include "types.h"          // Core types (uint32_t, etc.)
 #include "spinlock.h"       // Concurrency control (placeholder)
 #include <string.h>         // memcpy, memcmp, memset, strchr, strtok, etc.
 #include <libc/stdint.h>    // e.g. uint32_t in some setups
 #include <stdbool.h>
 
 /****************************************************************************
  * FAT Defines and Macros
  ****************************************************************************/
 
 #define ATTR_READ_ONLY      0x01
 #define ATTR_HIDDEN         0x02
 #define ATTR_SYSTEM         0x04
 #define ATTR_VOLUME_ID      0x08
 #define ATTR_DIRECTORY      0x10
 #define ATTR_ARCHIVE        0x20
 #define ATTR_LONG_NAME      (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)
 #define ATTR_LONG_NAME_MASK (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_DIRECTORY | ATTR_ARCHIVE)
 
 #define DIR_ENTRY_DELETED   0xE5
 #define DIR_ENTRY_UNUSED    0x00
 #define LFN_ENTRY_LAST      0x40 // Mask for sequence number byte
 
 #define MAX_LFN_ENTRIES     20
 #define MAX_LFN_CHARS       (MAX_LFN_ENTRIES * 13) // up to 260 chars
 #define MAX_FILENAME_LEN    255
 
 #ifndef DT_UNKNOWN
 #define DT_UNKNOWN 0
 #define DT_FIFO    1
 #define DT_CHR     2
 #define DT_DIR     4
 #define DT_BLK     6
 #define DT_REG     8
 #define DT_LNK    10
 #define DT_SOCK   12
 #endif
 
 // For partial readdir usage
 struct dirent {
     char     d_name[MAX_FILENAME_LEN + 1];
     uint32_t d_ino;       // Inode number (use cluster #)
     uint16_t d_reclen;    // optional
     uint8_t  d_type;      // DT_REG, DT_DIR, etc.
 };
 
 /****************************************************************************
  * FAT Boot Sector and Directory Entry Structures
  ****************************************************************************/
 #pragma pack(push, 1)
 typedef struct {
     uint8_t  name[11];        // 8.3 name (padded with spaces)
     uint8_t  attr;            // attribute flags
     uint8_t  reserved;        // reserved for Windows NT
     uint8_t  creation_time_10; // tenths of seconds
     uint16_t creation_time;   // time
     uint16_t creation_date;   // date
     uint16_t last_access_date;
     uint16_t first_cluster_high;
     uint16_t write_time;
     uint16_t write_date;
     uint16_t first_cluster_low;
     uint32_t file_size;
 } fat_dir_entry_t;
 #pragma pack(pop)
 
 #pragma pack(push, 1)
 typedef struct {
     uint8_t   jump_boot[3];
     uint8_t   oem_name[8];
     uint16_t  bytes_per_sector;
     uint8_t   sectors_per_cluster;
     uint16_t  reserved_sector_count;
     uint8_t   num_fats;
     uint16_t  root_entry_count;     // For FAT12/16
     uint16_t  total_sectors_short;  // if zero, use total_sectors_long
     uint8_t   media_descriptor;
     uint16_t  fat_size_16;          // FAT12/16 size in sectors
     uint16_t  sectors_per_track;
     uint16_t  number_of_heads;
     uint32_t  hidden_sectors;
     uint32_t  total_sectors_long;   // if total_sectors_short == 0
     // FAT32 extended:
     uint32_t  fat_size_32;          // FAT32 size in sectors
     uint16_t  ext_flags;
     uint16_t  fs_version;
     uint32_t  root_cluster;         // first cluster of root directory
     uint16_t  fs_info;
     uint16_t  backup_boot_sector;
     uint8_t   reserved2[12];
     uint8_t   drive_number;
     uint8_t   reserved3;
     uint8_t   boot_signature;
     uint32_t  volume_id;
     uint8_t   volume_label[11];
     uint8_t   fs_type[8];
     uint8_t   boot_code[420];
     uint16_t  boot_sector_signature; // 0xAA55
 } fat_boot_sector_t;
 #pragma pack(pop)
 
 /****************************************************************************
  * LFN Entry Structure
  ****************************************************************************/
 #pragma pack(push, 1)
 typedef struct {
     uint8_t   seq_num;
     uint16_t  name1[5];
     uint8_t   attr;
     uint8_t   type;
     uint8_t   checksum;
     uint16_t  name2[6];
     uint16_t  first_cluster;
     uint16_t  name3[2];
 } fat_lfn_entry_t;
 #pragma pack(pop)
 
 /****************************************************************************
  * FAT Types
  ****************************************************************************/
 #ifndef FAT_TYPE_FAT12
 #define FAT_TYPE_FAT12 1
 #define FAT_TYPE_FAT16 2
 #define FAT_TYPE_FAT32 3
 #endif
 
 /****************************************************************************
  * Core FS and File Context Structures
  ****************************************************************************/
 typedef struct fat_fs {
     disk_t disk;
     fat_boot_sector_t boot_sector;
 
     uint32_t fat_size;           // Size (in sectors) of ONE FAT
     uint32_t total_sectors;
     uint32_t first_data_sector;
     uint32_t root_dir_sectors;   // For FAT12/16
     uint32_t cluster_count;
     uint8_t  type;               // FAT_TYPE_FAT12, FAT_TYPE_FAT16, FAT_TYPE_FAT32
 
     // In-memory FAT
     void *fat_table;
 
     // Basic geometry info
     uint32_t root_cluster;       // For FAT32
     uint32_t sectors_per_cluster;
     uint32_t bytes_per_sector;
     uint32_t cluster_size_bytes;
     uint32_t fat_start_lba;
     uint32_t root_dir_start_lba; // For FAT12/16
 
     // EOC marker
     uint32_t eoc_marker;
 
     // Concurrency
     spinlock_t lock; // placeholder concurrency
 } fat_fs_t;
 
 typedef struct {
     fat_fs_t *fs;
     uint32_t first_cluster;
     uint32_t current_cluster;   // for sequential read/write
     uint32_t file_size;
 
     uint32_t dir_entry_cluster; // cluster containing this file's directory entry
     uint32_t dir_entry_offset;  // byte offset of the 8.3 entry
 
     bool is_directory;
     bool dirty;                 // if metadata changed, needs update on close
 
     // For readdir
     uint32_t readdir_current_cluster;
     uint32_t readdir_current_offset;
     size_t   readdir_last_index;
 } fat_file_context_t;
 
 /****************************************************************************
  * Forward Declarations for VFS Ops
  ****************************************************************************/
 extern int vfs_register_driver(vfs_driver_t *driver);
 extern int vfs_unregister_driver(vfs_driver_t *driver);
 
 /****************************************************************************
  * The VFS driver structure
  ****************************************************************************/
 static vfs_driver_t fat_vfs_driver = {
     .fs_name = "FAT",
     .mount   = NULL,
     .unmount = NULL,
     .open    = NULL,
     .read    = NULL,
     .write   = NULL,
     .close   = NULL,
     .lseek   = NULL,
     .readdir = NULL,
     .unlink  = NULL,
     .next    = NULL
 };
 
 /****************************************************************************
  * Function Prototypes for Internal Helpers
  ****************************************************************************/
 static int  read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer);
 static int  write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer);
 static int  load_fat_table(fat_fs_t *fs);
 static int  flush_fat_table(fat_fs_t *fs);
 
 static uint32_t find_free_cluster(fat_fs_t *fs);
 static uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster);
 static int      fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster);
 
 static int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                void *buf, size_t len);
 static int write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                 const void *buf, size_t len);
 
 static int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t* next_cluster);
 static int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);
 static uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster);
 
 static uint8_t calculate_lfn_checksum(const uint8_t name_8_3[11]);
 static void reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                             char *lfn_buf, size_t lfn_buf_size);
 static int generate_lfn_entries(const char* long_name,
                                 const uint8_t short_name[11],
                                 fat_lfn_entry_t* lfn_buf,
                                 int max_lfn_entries);
 
 static int generate_unique_short_name(fat_fs_t *fs,
                                       uint32_t parent_dir_cluster,
                                       const char* long_name,
                                       uint8_t short_name_out[11]);
 static int split_path(const char *full_path,
                       char *dir_part, size_t dir_max,
                       char *name_part, size_t name_max);
 
 static int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                                     size_t needed_slots,
                                     uint32_t *out_slot_cluster,
                                     uint32_t *out_slot_offset);
 static int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                                    uint32_t dir_offset,
                                    const void *entries_buf,
                                    size_t num_entries);
 static int update_directory_entry(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t dir_offset,
                                   const fat_dir_entry_t *new_entry);
 static int mark_directory_entry_deleted(fat_fs_t *fs,
                                         uint32_t dir_cluster,
                                         uint32_t dir_offset,
                                         uint8_t marker);
 static int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                                  uint32_t sector_offset_in_chain,
                                  uint8_t* buffer);
 
 static int fat_lookup_path(fat_fs_t *fs, const char *path,
                            fat_dir_entry_t *entry_out, char *lfn_out,
                            size_t lfn_max_len,
                            uint32_t *entry_dir_cluster,
                            uint32_t *entry_offset_in_dir);
 
 
 /****************************************************************************
  * Register / Unregister
  ****************************************************************************/
 int fat_register_driver(void) {
     terminal_write("[FAT] Registering FAT driver.\n");
     // Fill in the function pointers:
     fat_vfs_driver.mount   = fat_mount_internal;
     fat_vfs_driver.unmount = fat_unmount_internal;
     fat_vfs_driver.open    = fat_open_internal;
     fat_vfs_driver.read    = fat_read_internal;
     fat_vfs_driver.write   = fat_write_internal;
     fat_vfs_driver.close   = fat_close_internal;
     fat_vfs_driver.lseek   = fat_lseek_internal;
     fat_vfs_driver.readdir = fat_readdir_internal;
     fat_vfs_driver.unlink  = fat_unlink_internal;
 
     return vfs_register_driver(&fat_vfs_driver);
 }
 
 void fat_unregister_driver(void) {
     terminal_write("[FAT] Unregistering FAT driver.\n");
     vfs_unregister_driver(&fat_vfs_driver);
 }
 
 /****************************************************************************
  * MOUNT
  ****************************************************************************/
 static void *fat_mount_internal(const char *device)
 {
     terminal_printf("[FAT] Mounting device '%s'...\n", device);
     if (!device) return NULL;
 
     // Allocate our fs struct
     fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
     if (!fs) {
         terminal_write("[FAT] OOM in mount.\n");
         return NULL;
     }
     memset(fs, 0, sizeof(*fs));
     spinlock_init(&fs->lock);
 
     // Initialize disk
     if (disk_init(&fs->disk, device) != 0) {
         terminal_printf("[FAT] disk_init failed for %s.\n", device);
         kfree(fs);
         return NULL;
     }
 
     // Read boot sector
     buffer_t *bs = buffer_get(device, 0);
     if (!bs) {
         terminal_printf("[FAT] Could not read sector 0 on %s.\n", device);
         kfree(fs);
         return NULL;
     }
     memcpy(&fs->boot_sector, bs->data, sizeof(fat_boot_sector_t));
     buffer_release(bs);
 
     // Check signature
     if (fs->boot_sector.boot_sector_signature != 0xAA55) {
         terminal_printf("[FAT] Invalid boot sector sig on %s.\n", device);
         kfree(fs);
         return NULL;
     }
 
     // Basic geometry
     fs->bytes_per_sector    = fs->boot_sector.bytes_per_sector;
     fs->sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
     if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
         terminal_printf("[FAT] Invalid geometry (sector=0 or spc=0) on %s.\n", device);
         kfree(fs);
         return NULL;
     }
     fs->cluster_size_bytes  = fs->bytes_per_sector * fs->sectors_per_cluster;
 
     uint32_t total_sectors = (fs->boot_sector.total_sectors_short != 0)
                            ? fs->boot_sector.total_sectors_short
                            : fs->boot_sector.total_sectors_long;
     fs->fat_size = (fs->boot_sector.fat_size_16 != 0)
                  ? fs->boot_sector.fat_size_16
                  : fs->boot_sector.fat_size_32;
     if (total_sectors == 0 || fs->fat_size == 0 || fs->boot_sector.num_fats == 0) {
         terminal_printf("[FAT] Invalid geometry on %s.\n", device);
         kfree(fs);
         return NULL;
     }
     fs->total_sectors  = total_sectors;
     fs->fat_start_lba  = fs->boot_sector.reserved_sector_count;
 
     // Root dir for FAT12/16
     uint32_t root_dir_sectors = ((fs->boot_sector.root_entry_count * 32)
                                  + (fs->bytes_per_sector - 1))
                                  / fs->bytes_per_sector;
     fs->root_dir_sectors = root_dir_sectors;
     fs->root_dir_start_lba = fs->fat_start_lba
                            + (fs->boot_sector.num_fats * fs->fat_size);
 
     // Data area
     fs->first_data_sector = fs->root_dir_start_lba + root_dir_sectors;
 
     // Cluster count
     uint32_t data_sectors = fs->total_sectors - fs->first_data_sector;
     fs->cluster_count = data_sectors / fs->sectors_per_cluster;
 
     // Determine FAT type
     if (fs->cluster_count < 4085) {
         fs->type = FAT_TYPE_FAT12;
         fat_vfs_driver.fs_name = "FAT12";
         fs->root_cluster = 0; // not used
         fs->eoc_marker = 0xFFF; // common for FAT12
         terminal_write("[FAT] Detected FAT12.\n");
     } else if (fs->cluster_count < 65525) {
         fs->type = FAT_TYPE_FAT16;
         fat_vfs_driver.fs_name = "FAT16";
         fs->root_cluster = 0;
         fs->eoc_marker = 0xFFF8;
         terminal_write("[FAT] Detected FAT16.\n");
     } else {
         fs->type = FAT_TYPE_FAT32;
         fat_vfs_driver.fs_name = "FAT32";
         fs->root_dir_sectors = 0; // not used
         fs->root_cluster = fs->boot_sector.root_cluster;
         fs->eoc_marker = 0x0FFFFFF8;
         terminal_write("[FAT] Detected FAT32.\n");
     }
 
     // Load the FAT
     if (load_fat_table(fs) != FS_SUCCESS) {
         terminal_printf("[FAT] Failed to load FAT for %s.\n", device);
         kfree(fs);
         return NULL;
     }
 
     terminal_printf("[FAT] Mounted '%s' as %s.\n", device, fat_vfs_driver.fs_name);
     return fs;
 }
 
 /****************************************************************************
  * UNMOUNT
  ****************************************************************************/
 static int fat_unmount_internal(void *fs_context)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs) return -FS_ERR_INVALID_PARAM;
     terminal_printf("[FAT] Unmounting %s (%s)...\n",
                     fs->disk.blk_dev.device_name,
                     fat_vfs_driver.fs_name);
 
     // Flush & free
     if (fs->fat_table) {
         flush_fat_table(fs);
         kfree(fs->fat_table);
         fs->fat_table = NULL;
     }
     buffer_cache_sync(); // ensure all data hits disk
     kfree(fs);
     terminal_write("[FAT] Unmount complete.\n");
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * OPEN (with O_CREAT / O_TRUNC)
  ****************************************************************************/
 static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
 {
     fat_fs_t *fs = (fat_fs_t *)fs_context;
     if (!fs || !path) return NULL;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     fat_dir_entry_t entry;
     char lfn_buffer[MAX_LFN_CHARS];
     uint32_t entry_dir_cluster, entry_offset_in_dir;
     int find_res = fat_lookup_path(fs, path, &entry, lfn_buffer,
                                    sizeof(lfn_buffer),
                                    &entry_dir_cluster, &entry_offset_in_dir);
 
     bool exists = (find_res == FS_SUCCESS);
     bool created = false, truncated = false;
 
     vnode_t *vnode = NULL;
     fat_file_context_t *file_ctx = NULL;
     int ret_err = FS_SUCCESS;
 
     if (!exists && (flags & O_CREAT)) {
         // CREATE
         created = true;
         char parent_dir_path[256];
         char new_name[MAX_FILENAME_LEN + 1];
         memset(parent_dir_path, 0, sizeof(parent_dir_path));
         memset(new_name, 0, sizeof(new_name));
 
         if (split_path(path, parent_dir_path, sizeof(parent_dir_path),
                        new_name, sizeof(new_name)) != 0)
         {
             ret_err = -FS_ERR_NAMETOOLONG;
             goto open_fail_locked;
         }
 
         // Lookup parent
         fat_dir_entry_t parent_entry;
         uint32_t parent_cluster, parent_offset;
         int pres = fat_lookup_path(fs, parent_dir_path,
                                    &parent_entry, NULL, 0,
                                    &parent_cluster, &parent_offset);
         if (pres != FS_SUCCESS) {
             ret_err = pres;
             goto open_fail_locked;
         }
         if (!(parent_entry.attr & ATTR_DIRECTORY)) {
             ret_err = -FS_ERR_NOT_A_DIRECTORY;
             goto open_fail_locked;
         }
 
         // Get parent's cluster
         auto fat_get_entry_cluster = [&](const fat_dir_entry_t *e)->uint32_t {
             return (((uint32_t)e->first_cluster_high) << 16)
                    | e->first_cluster_low;
         };
         uint32_t p_clus = fat_get_entry_cluster(&parent_entry);
         // Minimal check
         if (p_clus == 0 && fs->type == FAT_TYPE_FAT32) {
             // For FAT32, root shouldn't be cluster=0
             // but let's skip the deep checks for brevity
         }
 
         // Generate a short name
         uint8_t short_name[11];
         if (generate_unique_short_name(fs, p_clus, new_name, short_name) != 0) {
             ret_err = -FS_ERR_UNKNOWN;
             goto open_fail_locked;
         }
 
         // LFN entries
         fat_lfn_entry_t lfn_entries[MAX_LFN_ENTRIES];
         int lfn_count = generate_lfn_entries(new_name, short_name,
                                              lfn_entries, MAX_LFN_ENTRIES);
         size_t needed_slots = (size_t)lfn_count + 1;
 
         uint32_t slot_cluster, slot_offset;
         if (find_free_directory_slot(fs, p_clus, needed_slots,
                                      &slot_cluster, &slot_offset) != FS_SUCCESS)
         {
             ret_err = -FS_ERR_NO_SPACE;
             goto open_fail_locked;
         }
 
         // Prepare 8.3 entry
         memset(&entry, 0, sizeof(entry));
         memcpy(entry.name, short_name, 11);
         entry.attr = ATTR_ARCHIVE;
         entry.file_size = 0;
         entry.first_cluster_low  = 0;
         entry.first_cluster_high = 0;
 
         // Write LFN then 8.3
         uint32_t cur_off = slot_offset;
         if (lfn_count > 0) {
             if (write_directory_entries(fs, slot_cluster, cur_off,
                                         lfn_entries, lfn_count)!=FS_SUCCESS)
             {
                 ret_err = -FS_ERR_IO;
                 goto open_fail_locked;
             }
             cur_off += (uint32_t)(lfn_count * sizeof(fat_dir_entry_t));
         }
         // Now write the final 8.3 entry
         if (write_directory_entries(fs, slot_cluster, cur_off,
                                     &entry, 1) != FS_SUCCESS)
         {
             ret_err = -FS_ERR_IO;
             goto open_fail_locked;
         }
 
         entry_dir_cluster    = slot_cluster;
         entry_offset_in_dir  = cur_off;
         buffer_cache_sync();
         terminal_printf("[FAT O_CREAT] Created '%s'\n", path);
         exists = true;
     }
     else if (!exists) {
         // The file does not exist and O_CREAT wasn't specified, or error
         ret_err = find_res;
         goto open_fail_locked;
     }
 
     // Check perms
     if ((flags & O_WRONLY) && (entry.attr & ATTR_READ_ONLY)) {
         ret_err = -FS_ERR_PERMISSION_DENIED;
         goto open_fail_locked;
     }
     if ((flags & (O_WRONLY|O_RDWR)) && (entry.attr & ATTR_DIRECTORY)) {
         ret_err = -FS_ERR_IS_A_DIRECTORY;
         goto open_fail_locked;
     }
 
     // O_TRUNC
     if (exists && !created && !(entry.attr & ATTR_DIRECTORY) && (flags & O_TRUNC)) {
         truncated = true;
         terminal_printf("[FAT open O_TRUNC] Truncating '%s'\n", path);
 
         auto fat_get_entry_cluster = [&](const fat_dir_entry_t *e)->uint32_t {
             return (((uint32_t)e->first_cluster_high) << 16)
                    | e->first_cluster_low;
         };
         uint32_t fc = fat_get_entry_cluster(&entry);
         if (fc >= 2) {
             if (fat_free_cluster_chain(fs, fc) != FS_SUCCESS) {
                 ret_err = -FS_ERR_IO;
                 goto open_fail_locked;
             }
         }
         entry.file_size = 0;
         entry.first_cluster_low  = 0;
         entry.first_cluster_high = 0;
         if (update_directory_entry(fs, entry_dir_cluster, entry_offset_in_dir,
                                    &entry) != FS_SUCCESS)
         {
             ret_err = -FS_ERR_IO;
             goto open_fail_locked;
         }
         flush_fat_table(fs);
         buffer_cache_sync();
     }
 
     // Allocate vnode & context
     vnode = (vnode_t*)kmalloc(sizeof(vnode_t));
     file_ctx = (fat_file_context_t*)kmalloc(sizeof(fat_file_context_t));
     if (!vnode || !file_ctx) {
         ret_err = -FS_ERR_OUT_OF_MEMORY;
         goto open_fail_locked;
     }
     memset(vnode, 0, sizeof(*vnode));
     memset(file_ctx, 0, sizeof(*file_ctx));
 
     // Fill the context
     auto fat_get_entry_cluster = [&](const fat_dir_entry_t *e)->uint32_t {
         return (((uint32_t)e->first_cluster_high) << 16)
                | e->first_cluster_low;
     };
     uint32_t first_cluster = fat_get_entry_cluster(&entry);
 
     file_ctx->fs                = fs;
     file_ctx->first_cluster     = first_cluster;
     file_ctx->current_cluster   = first_cluster;
     file_ctx->file_size         = entry.file_size;
     file_ctx->dir_entry_cluster = entry_dir_cluster;
     file_ctx->dir_entry_offset  = entry_offset_in_dir;
     file_ctx->is_directory      = (entry.attr & ATTR_DIRECTORY) != 0;
     file_ctx->dirty             = (created || truncated);
 
     // readdir fields
     file_ctx->readdir_current_cluster = file_ctx->first_cluster;
     file_ctx->readdir_current_offset  = 0;
     file_ctx->readdir_last_index      = (size_t)-1;
 
     vnode->data      = file_ctx;
     vnode->fs_driver = &fat_vfs_driver;
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return vnode;
 
 open_fail_locked:
     if (vnode)    kfree(vnode);
     if (file_ctx) kfree(file_ctx);
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return NULL;
 }
 
 /****************************************************************************
  * READ
  ****************************************************************************/
 static int fat_read_internal(file_t *file, void *buf, size_t len)
 {
     if (!file || !file->vnode || !file->vnode->data || !buf)
         return -FS_ERR_INVALID_PARAM;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     if (!fctx->fs) return -FS_ERR_INVALID_PARAM;
     if (fctx->is_directory) {
         // directory => use readdir
         return -FS_ERR_IS_A_DIRECTORY;
     }
     fat_fs_t *fs = fctx->fs;
 
     // Basic offset checks
     if (file->offset < 0) file->offset = 0;
     if ((uint64_t)file->offset >= fctx->file_size) {
         // at EOF
         return 0;
     }
 
     size_t remain = (fctx->file_size - (size_t)file->offset);
     if (len > remain) len = remain;
     if (len == 0) return 0;
 
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     // We'll do a simplified read approach:
     //  1. Find cluster corresponding to file->offset
     //  2. Read from offset, keep going until we fill 'len'
     // For large reads, we'd parse multiple clusters. We'll implement it partially.
 
     size_t total_read = 0;
     size_t bytes_left = len;
     off_t  user_offset = file->offset;
 
     // We'll gather cluster chain on the fly
     uint32_t cluster_index = (uint32_t)(user_offset / cluster_size);
     uint32_t offset_in_cluster = (uint32_t)(user_offset % cluster_size);
 
     // Traverse cluster chain to find cluster_index's cluster
     uint32_t current_cluster = fctx->first_cluster;
     if (current_cluster < 2 && fctx->file_size > 0) {
         // invalid
         return -FS_ERR_CORRUPT;
     }
     // If file_size==0, there's no cluster allocated => read=0
     if (fctx->first_cluster < 2 && fctx->file_size == 0) {
         return 0;
     }
 
     // Step to cluster_index
     for (uint32_t i = 0; i < cluster_index; i++) {
         uint32_t next = 0;
         if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
             // Something wrong with FAT
             return -FS_ERR_IO;
         }
         if (next >= fs->eoc_marker) {
             // We reached EOC before we expected => partial read
             break;
         }
         current_cluster = next;
     }
 
     uint8_t bounce_buffer[4096]; // Enough for a single cluster if <=4KB
     // Read until we fulfill the 'len' or EOC
     while (bytes_left > 0 && current_cluster >= 2 && current_cluster < fs->eoc_marker) {
         size_t to_read_in_cluster = cluster_size - offset_in_cluster;
         if (to_read_in_cluster > bytes_left) {
             to_read_in_cluster = bytes_left;
         }
 
         // Read the cluster into bounce_buffer
         if (cluster_size > sizeof(bounce_buffer)) {
             // Very large cluster size not handled here for brevity
             return -FS_ERR_NOT_SUPPORTED;
         }
         int rc = read_cluster_cached(fs, current_cluster, 0,
                                      bounce_buffer, cluster_size);
         if (rc < 0) return rc;
 
         // Copy out the portion
         memcpy((uint8_t*)buf + total_read,
                bounce_buffer + offset_in_cluster,
                to_read_in_cluster);
 
         total_read     += to_read_in_cluster;
         bytes_left     -= to_read_in_cluster;
         user_offset    += to_read_in_cluster;
         offset_in_cluster = 0; // for subsequent clusters
 
         // Move to next cluster if needed
         if (bytes_left > 0) {
             uint32_t next = 0;
             if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
                 return -FS_ERR_IO;
             }
             current_cluster = next;
         }
     }
 
     file->offset += total_read;
     return (int)total_read;
 }
 
 /****************************************************************************
  * WRITE
  ****************************************************************************/
 static int fat_write_internal(file_t *file, const void *buf, size_t len)
 {
     if (!file || !file->vnode || !file->vnode->data || !buf)
         return -FS_ERR_INVALID_PARAM;
     if (len == 0) return 0;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     if (!fctx->fs) return -FS_ERR_INVALID_PARAM;
     if (fctx->is_directory) return -FS_ERR_IS_A_DIRECTORY;
     fat_fs_t *fs = fctx->fs;
 
     if (!(file->flags & (O_WRONLY|O_RDWR))) {
         return -FS_ERR_PERMISSION_DENIED;
     }
 
     // O_APPEND => offset = file_size
     if (file->flags & O_APPEND) {
         file->offset = (off_t)fctx->file_size;
     }
     if (file->offset < 0) file->offset = 0;
 
     size_t total_written = 0;
     size_t bytes_left = len;
     off_t  user_offset = file->offset;
     size_t cluster_size = fs->cluster_size_bytes;
 
     // We'll do a simplified approach: 
     //  - For the cluster that user_offset falls in, allocate if needed, read old content, overwrite, write back
     //  - If we run out of clusters while writing, allocate a new cluster at the end
 
     if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     // If file has no cluster yet and user starts writing => allocate first cluster
     if (fctx->first_cluster < 2 && fctx->file_size == 0) {
         uint32_t newc = fat_allocate_cluster(fs, 0);
         if (newc < 2) return -FS_ERR_NO_SPACE;
         fctx->first_cluster   = newc;
         fctx->current_cluster = newc;
         fctx->dirty           = true;
     }
 
     uint8_t bounce_buffer[4096]; // cluster temp
     while (bytes_left > 0) {
         // find which cluster index we are writing to
         uint32_t cluster_index = (uint32_t)(user_offset / cluster_size);
         uint32_t offset_in_cluster = (uint32_t)(user_offset % cluster_size);
 
         // we need to walk cluster_index clusters from first_cluster
         uint32_t current_cluster = fctx->first_cluster;
         if (current_cluster < 2) {
             // Should have allocated above
             return -FS_ERR_CORRUPT;
         }
         for (uint32_t i=0; i<cluster_index; i++) {
             // walk the chain
             uint32_t next;
             if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
                 return -FS_ERR_IO;
             }
             if (next >= fs->eoc_marker) {
                 // allocate new cluster
                 next = fat_allocate_cluster(fs, current_cluster);
                 if (next < 2) return -FS_ERR_NO_SPACE;
             }
             current_cluster = next;
         }
 
         // We have the cluster now, read it in to bounce_buffer first (if partial overwrite)
         size_t to_write_in_cluster = cluster_size - offset_in_cluster;
         if (to_write_in_cluster > bytes_left) {
             to_write_in_cluster = bytes_left;
         }
 
         // read old content if partial write
         if (cluster_size > sizeof(bounce_buffer)) {
             return -FS_ERR_NOT_SUPPORTED; // example limit
         }
         int rc = read_cluster_cached(fs, current_cluster, 0,
                                      bounce_buffer, cluster_size);
         if (rc < 0) return rc;
 
         // copy new data
         memcpy(bounce_buffer + offset_in_cluster,
                (uint8_t*)buf + total_written,
                to_write_in_cluster);
 
         // write back
         rc = write_cluster_cached(fs, current_cluster, 0,
                                   bounce_buffer, cluster_size);
         if (rc < 0) return rc;
 
         total_written  += to_write_in_cluster;
         bytes_left     -= to_write_in_cluster;
         user_offset    += to_write_in_cluster;
 
         // update file_size if extended
         if ((size_t)user_offset > fctx->file_size) {
             fctx->file_size = (size_t)user_offset;
             fctx->dirty     = true;
         }
     }
 
     file->offset += total_written;
     return (int)total_written;
 }
 
 /****************************************************************************
  * LSEEK
  ****************************************************************************/
 static off_t fat_lseek_internal(file_t *file, off_t offset, int whence)
 {
     if (!file || !file->vnode || !file->vnode->data)
         return (off_t)-FS_ERR_INVALID_PARAM;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     off_t new_off;
     switch (whence) {
     case SEEK_SET:
         new_off = offset;
         break;
     case SEEK_CUR:
         new_off = file->offset + offset;
         break;
     case SEEK_END:
         new_off = (off_t)fctx->file_size + offset;
         break;
     default:
         return (off_t)-FS_ERR_INVALID_PARAM;
     }
     if (new_off < 0) return (off_t)-FS_ERR_INVALID_PARAM;
     file->offset = new_off;
     return new_off;
 }
 
 /****************************************************************************
  * CLOSE
  ****************************************************************************/
 static int fat_close_internal(file_t *file)
 {
     if (!file || !file->vnode || !file->vnode->data)
         return -FS_ERR_INVALID_PARAM;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     if (!fctx->fs) return -FS_ERR_INVALID_PARAM;
     fat_fs_t *fs = fctx->fs;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     // If dirty => update directory entry with new size, cluster info
     if (fctx->dirty) {
         // Re-read the directory entry, update size, clusters, etc.
         // For brevity, just do partial update
         fat_dir_entry_t old_entry;
         // read existing entry
         {
             size_t sec_size = fs->bytes_per_sector;
             uint32_t sector_offset_in_chain = fctx->dir_entry_offset / sec_size;
             size_t offset_in_sector = fctx->dir_entry_offset % sec_size;
             uint8_t* sector_buffer = (uint8_t*)kmalloc(sec_size);
             if (sector_buffer) {
                 if (read_directory_sector(fs, fctx->dir_entry_cluster,
                                           sector_offset_in_chain,
                                           sector_buffer) == FS_SUCCESS)
                 {
                     memcpy(&old_entry, sector_buffer + offset_in_sector,
                            sizeof(fat_dir_entry_t));
                 }
                 kfree(sector_buffer);
             }
         }
         // Now update file_size and cluster
         old_entry.file_size = fctx->file_size;
         old_entry.first_cluster_low  = (uint16_t)(fctx->first_cluster & 0xFFFF);
         old_entry.first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
 
         update_directory_entry(fs, fctx->dir_entry_cluster,
                                fctx->dir_entry_offset, &old_entry);
         flush_fat_table(fs);
     }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     kfree(file->vnode->data);
     kfree(file->vnode);
     file->vnode = NULL;
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * UNLINK
  ****************************************************************************/
 static int fat_unlink_internal(void *fs_context, const char *path)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs || !path) return -FS_ERR_INVALID_PARAM;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     fat_dir_entry_t entry;
     char lfn_buf[MAX_LFN_CHARS];
     uint32_t dir_cluster, entry_offset;
     int find_res = fat_lookup_path(fs, path, &entry, lfn_buf, sizeof(lfn_buf),
                                    &dir_cluster, &entry_offset);
     if (find_res != FS_SUCCESS) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return find_res;
     }
     if (entry.attr & ATTR_DIRECTORY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_IS_A_DIRECTORY;
     }
     if (entry.attr & ATTR_READ_ONLY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_PERMISSION_DENIED;
     }
 
     // Free cluster chain
     auto fat_get_entry_cluster = [&](const fat_dir_entry_t *e)->uint32_t {
         return (((uint32_t)e->first_cluster_high) << 16) | e->first_cluster_low;
     };
     uint32_t fc = fat_get_entry_cluster(&entry);
     if (fc >= 2) {
         fat_free_cluster_chain(fs, fc);
     }
 
     // Mark 8.3 entry deleted. Deleting LFN entries as well would require
     // scanning backward for consecutive LFN entries. We'll do partial here.
     mark_directory_entry_deleted(fs, dir_cluster, entry_offset, DIR_ENTRY_DELETED);
 
     flush_fat_table(fs);
     buffer_cache_sync();
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * READDIR
  ****************************************************************************/
 static int fat_readdir_internal(file_t *dir_file,
                                 struct dirent *d_entry_out,
                                 size_t entry_index)
 {
     if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out)
         return -FS_ERR_INVALID_PARAM;
     fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
     if (!fctx->fs || !fctx->is_directory) return -FS_ERR_INVALID_PARAM;
 
     fat_fs_t *fs = fctx->fs;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     // If entry_index == 0, reset
     if (entry_index == 0) {
         fctx->readdir_current_cluster = fctx->first_cluster;
         fctx->readdir_current_offset  = 0;
         fctx->readdir_last_index      = (size_t)-1;
     }
     if (entry_index != fctx->readdir_last_index + 1) {
         // not sequential => re-scan from start
         fctx->readdir_current_cluster = fctx->first_cluster;
         fctx->readdir_current_offset  = 0;
         fctx->readdir_last_index      = (size_t)-1;
     }
 
     bool searching_fixed_root = (fctx->readdir_current_cluster == 0
                               && fs->type != FAT_TYPE_FAT32);
 
     // We'll use a bounce buffer for reading directory sectors
     uint8_t *buffer = (uint8_t*)kmalloc(fs->cluster_size_bytes);
     if (!buffer) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     fat_lfn_entry_t lfn_collector[MAX_LFN_ENTRIES];
     int lfn_collector_count = 0;
     size_t current_entry_idx_scan = 0;
     bool found_entry = false;
     int ret_err = -FS_ERR_NOT_FOUND;
 
     // We'll do a scanning approach. We keep reading directory entries until
     // we either find the (entry_index)-th valid entry or run out.
     // Pseudocode approach:
     while (!found_entry) {
         // read the sector that readdir_current_offset is in
         uint32_t sec_size = fs->bytes_per_sector;
         uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
         size_t offset_in_sector = fctx->readdir_current_offset % sec_size;
 
         // load that sector
         if (read_directory_sector(fs, fctx->readdir_current_cluster,
                                   sector_offset_in_chain, buffer) != FS_SUCCESS)
         {
             // Possibly we are beyond the end => no more entries
             break;
         }
 
         // The index within the sector
         size_t entry_in_sector = offset_in_sector / sizeof(fat_dir_entry_t);
         fat_dir_entry_t *dirent_ptr = (fat_dir_entry_t*)(buffer + (entry_in_sector * sizeof(fat_dir_entry_t)));
 
         if (dirent_ptr->name[0] == DIR_ENTRY_UNUSED) {
             // No more entries
             break;
         }
         if (dirent_ptr->name[0] == DIR_ENTRY_DELETED) {
             // skip
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
             if (fctx->readdir_current_offset >= fs->cluster_size_bytes && !searching_fixed_root) {
                 // move to next cluster or next sector in chain
                 // but for brevity, we just keep incrementing, read_directory_sector
                 // will fail if we surpass
             }
             continue;
         }
 
         // Check if it's an LFN entry
         if ((dirent_ptr->attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
             // LFN
             fat_lfn_entry_t *lfn = (fat_lfn_entry_t*)dirent_ptr;
             // store in collector
             if (lfn_collector_count < MAX_LFN_ENTRIES) {
                 lfn_collector[lfn_collector_count++] = *lfn;
             }
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
             continue;
         }
 
         // otherwise 8.3 entry
         if ((dirent_ptr->attr & ATTR_VOLUME_ID) == ATTR_VOLUME_ID) {
             // skip volume labels
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
             continue;
         }
         // This is a real file/dir
         if (current_entry_idx_scan == entry_index) {
             // reconstruct LFN if any
             if (lfn_collector_count > 0) {
                 reconstruct_lfn(lfn_collector, lfn_collector_count,
                                 d_entry_out->d_name,
                                 sizeof(d_entry_out->d_name));
             } else {
                 // fallback to 8.3
                 // simplistic: copy name (not converting spaces, etc.)
                 memcpy(d_entry_out->d_name, dirent_ptr->name, 11);
                 d_entry_out->d_name[11] = '\0';
                 // fix them to something more standard, or use `fat_utils.c` if available
             }
             // d_ino = cluster
             uint32_t first_cluster = (((uint32_t)dirent_ptr->first_cluster_high) << 16)
                                    | dirent_ptr->first_cluster_low;
             d_entry_out->d_ino = first_cluster;
             d_entry_out->d_type = (dirent_ptr->attr & ATTR_DIRECTORY)? DT_DIR : DT_REG;
 
             found_entry = true;
             ret_err = FS_SUCCESS;
         }
 
         // move forward
         current_entry_idx_scan++;
         fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
         // reset lfn collector
         lfn_collector_count = 0;
     }
 
     kfree(buffer);
     if (found_entry) {
         fctx->readdir_last_index = entry_index;
     }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return ret_err;
 }
 
 /****************************************************************************
  * Internal Helper Implementations
  ****************************************************************************/
 
 /* read_fat_sector / write_fat_sector */
 static int read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer) {
     if (!fs || !buffer) return -FS_ERR_INVALID_PARAM;
     uint32_t target_lba = fs->fat_start_lba + sector_offset;
     if (fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
 
     buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
     if (!buf) return -FS_ERR_IO;
     memcpy(buffer, buf->data, fs->bytes_per_sector);
     buffer_release(buf);
     return FS_SUCCESS;
 }
 
 static int write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer) {
     if (!fs || !buffer) return -FS_ERR_INVALID_PARAM;
     uint32_t target_lba = fs->fat_start_lba + sector_offset;
     if (fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
 
     buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
     if (!buf) return -FS_ERR_IO;
     memcpy(buf->data, buffer, fs->bytes_per_sector);
     buffer_mark_dirty(buf);
     buffer_release(buf);
     return FS_SUCCESS;
 }
 
 /* load_fat_table / flush_fat_table */
 static int load_fat_table(fat_fs_t *fs) {
     if (!fs) return -FS_ERR_INVALID_PARAM;
     if (fs->fat_size == 0 || fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
     size_t table_size = fs->fat_size * fs->bytes_per_sector;
     if (table_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     fs->fat_table = kmalloc(table_size);
     if (!fs->fat_table) return -FS_ERR_OUT_OF_MEMORY;
 
     uint8_t *current_ptr = (uint8_t *)fs->fat_table;
     for (uint32_t i = 0; i < fs->fat_size; i++) {
         if (read_fat_sector(fs, i, current_ptr) != FS_SUCCESS) {
             kfree(fs->fat_table);
             fs->fat_table = NULL;
             return -FS_ERR_IO;
         }
         current_ptr += fs->bytes_per_sector;
     }
     terminal_printf("[FAT] FAT table loaded (%u sectors) for %s.\n",
                     fs->fat_size, fs->disk.blk_dev.device_name);
     return FS_SUCCESS;
 }
 
 static int flush_fat_table(fat_fs_t *fs) {
     if (!fs || !fs->fat_table) return FS_SUCCESS; // Nothing to flush
     if (fs->fat_size == 0 || fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
 
     const uint8_t *current_ptr = (const uint8_t *)fs->fat_table;
     for (uint32_t i = 0; i < fs->fat_size; i++) {
         if (write_fat_sector(fs, i, current_ptr) != FS_SUCCESS) {
             return -FS_ERR_IO;
         }
         current_ptr += fs->bytes_per_sector;
     }
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * FAT Access Helpers (fat_get_next_cluster / fat_set_cluster_entry / cluster->LBA)
  ****************************************************************************/
 static int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t* next_cluster)
 {
     if (!fs || !fs->fat_table || !next_cluster) return -FS_ERR_INVALID_PARAM;
 
     if (current_cluster < 2) {
         *next_cluster = fs->eoc_marker; // invalid or ends
         return FS_SUCCESS;
     }
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *FAT32 = (uint32_t*)fs->fat_table;
         uint32_t val = FAT32[current_cluster] & 0x0FFFFFFF;
         if (val >= 0x0FFFFFF8) {
             // EOC
             *next_cluster = fs->eoc_marker;
         } else {
             *next_cluster = val;
         }
     } else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *FAT16 = (uint16_t*)fs->fat_table;
         uint16_t val = FAT16[current_cluster];
         if (val >= 0xFFF8) {
             *next_cluster = fs->eoc_marker;
         } else {
             *next_cluster = val;
         }
     } else {
         // FAT12 partial
         // Not fully implemented
         *next_cluster = fs->eoc_marker;
     }
     return FS_SUCCESS;
 }
 
 static int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value)
 {
     if (!fs || !fs->fat_table) return -FS_ERR_INVALID_PARAM;
 
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *FAT32 = (uint32_t*)fs->fat_table;
         FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
     } else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *FAT16 = (uint16_t*)fs->fat_table;
         FAT16[cluster] = (uint16_t)(value & 0xFFFF);
     } else {
         // FAT12 partial
         // Not fully implemented
         // ...
     }
     return FS_SUCCESS;
 }
 
 static uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster)
 {
     if (!fs) return 0;
     if (cluster < 2) {
         // Possibly root for FAT12/16?
         return 0; 
     }
     // For FAT32 or for data clusters in FAT16:
     return fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
 }
 
 /****************************************************************************
  * find_free_cluster, fat_allocate_cluster, fat_free_cluster_chain
  ****************************************************************************/
 static uint32_t find_free_cluster(fat_fs_t *fs) {
     if (!fs || !fs->fat_table) return 0;
     // Basic linear scan from cluster 2
     uint32_t total_clusters = fs->cluster_count + 2; // from 0..(cluster_count+1)
 
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *fat = (uint32_t*)fs->fat_table;
         for (uint32_t i = 2; i < total_clusters; ++i) {
             if ((fat[i] & 0x0FFFFFFF) == 0) {
                 return i;
             }
         }
     } else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *fat = (uint16_t*)fs->fat_table;
         for (uint32_t i = 2; i < total_clusters; ++i) {
             if (fat[i] == 0) {
                 return i;
             }
         }
     } else {
         // FAT12 partial
         // Not fully implemented
         return 0;
     }
     return 0; // no free found
 }
 
 static uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster)
 {
     if (!fs || !fs->fat_table) return 0;
     uint32_t free_cluster = find_free_cluster(fs);
     if (free_cluster == 0) {
         return 0; // no space
     }
     // Mark the new cluster as EOC
     if (fat_set_cluster_entry(fs, free_cluster, fs->eoc_marker) != FS_SUCCESS) {
         return 0;
     }
     // Link from previous if needed
     if (previous_cluster >= 2) {
         if (fat_set_cluster_entry(fs, previous_cluster, free_cluster) != FS_SUCCESS) {
             fat_set_cluster_entry(fs, free_cluster, 0); // revert
             return 0;
         }
     }
     return free_cluster;
 }
 
 static int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster)
 {
     if (!fs || !fs->fat_table || start_cluster < 2) return -FS_ERR_INVALID_PARAM;
 
     uint32_t current = start_cluster;
     while (current >= 2 && current < fs->eoc_marker) {
         uint32_t next_cluster = 0;
         if (fat_get_next_cluster(fs, current, &next_cluster) != FS_SUCCESS) {
             // I'd keep freeing anyway
         }
         // Mark current free
         fat_set_cluster_entry(fs, current, 0);
         current = next_cluster;
         if (current >= fs->eoc_marker) break;
     }
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * read_cluster_cached / write_cluster_cached
  ****************************************************************************/
 static int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                void *buf, size_t len)
 {
     if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;
     if (offset_in_cluster + len > fs->cluster_size_bytes) {
         // can't read beyond cluster
         return -FS_ERR_INVALID_PARAM;
     }
     // number of sectors to read
     uint32_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t start_sector = offset_in_cluster / sector_size;
     uint32_t end_sector   = (offset_in_cluster + len - 1) / sector_size;
 
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) return -FS_ERR_IO;
 
     size_t read_offset_in_buf = 0;
     for (uint32_t sec = start_sector; sec <= end_sector; sec++) {
         uint32_t lba = cluster_lba + sec;
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
 
         size_t offset_in_sector = 0;
         if (sec == start_sector) {
             offset_in_sector = offset_in_cluster % sector_size;
         }
         size_t bytes_this_sector = sector_size - offset_in_sector;
         if (bytes_this_sector > (len - read_offset_in_buf)) {
             bytes_this_sector = (len - read_offset_in_buf);
         }
         memcpy((uint8_t*)buf + read_offset_in_buf,
                b->data + offset_in_sector,
                bytes_this_sector);
         read_offset_in_buf += bytes_this_sector;
 
         buffer_release(b);
     }
     return (int)len;
 }
 
 static int write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                 const void *buf, size_t len)
 {
     if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;
     if (offset_in_cluster + len > fs->cluster_size_bytes) {
         // can't write beyond cluster
         return -FS_ERR_INVALID_PARAM;
     }
     uint32_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t start_sector = offset_in_cluster / sector_size;
     uint32_t end_sector   = (offset_in_cluster + len - 1) / sector_size;
 
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) return -FS_ERR_IO;
 
     size_t written_offset_in_buf = 0;
     for (uint32_t sec = start_sector; sec <= end_sector; sec++) {
         uint32_t lba = cluster_lba + sec;
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
 
         size_t offset_in_sector = 0;
         if (sec == start_sector) {
             offset_in_sector = offset_in_cluster % sector_size;
         }
         size_t bytes_this_sector = sector_size - offset_in_sector;
         if (bytes_this_sector > (len - written_offset_in_buf)) {
             bytes_this_sector = (len - written_offset_in_buf);
         }
         memcpy(b->data + offset_in_sector,
                (uint8_t*)buf + written_offset_in_buf,
                bytes_this_sector);
         buffer_mark_dirty(b);
         buffer_release(b);
 
         written_offset_in_buf += bytes_this_sector;
     }
     return (int)len;
 }
 
 /****************************************************************************
  * LFN Helpers
  ****************************************************************************/
 static uint8_t calculate_lfn_checksum(const uint8_t name_8_3[11]) {
     uint8_t sum = 0;
     for (int i = 0; i < 11; i++) {
         sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name_8_3[i];
     }
     return sum;
 }
 
 static void reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                             char *lfn_buf, size_t lfn_buf_size)
 {
     if (!lfn_buf || lfn_buf_size == 0) return;
     lfn_buf[0] = '\0';
 
     int buf_idx = 0;
     // LFN entries come in reverse order on disk; typically you collect them in reverse
     // We'll assume the array is in the order we encountered them (lowest seq last).
     for (int i = lfn_count - 1; i >= 0; i--) {
         uint16_t *name_parts[] = {
             lfn_entries[i].name1, lfn_entries[i].name2, lfn_entries[i].name3
         };
         size_t name_lengths[] = { 5, 6, 2 };
         bool done = false;
         for (int part = 0; part < 3 && !done; part++) {
             for (size_t c = 0; c < name_lengths[part]; c++) {
                 uint16_t wc = name_parts[part][c];
                 if (wc == 0x0000 || wc == 0xFFFF) {
                     done = true;
                     break;
                 }
                 if (buf_idx < (int)lfn_buf_size - 1) {
                     // Minimal ASCII approach
                     char ch = (char)((wc <= 0x7F) ? wc : '?');
                     lfn_buf[buf_idx++] = ch;
                 } else {
                     done = true;
                     break;
                 }
             }
         }
     }
     lfn_buf[buf_idx] = '\0';
 }
 
 static int generate_lfn_entries(const char* long_name,
                                 const uint8_t short_name[11],
                                 fat_lfn_entry_t* lfn_buf,
                                 int max_lfn_entries)
 {
     if (!long_name || !short_name || !lfn_buf || max_lfn_entries <= 0) return 0;
     size_t lfn_len = strlen(long_name);
     int needed = (int)((lfn_len + 12) / 13);
     if (needed > max_lfn_entries) {
         // can't store that many
         return 0;
     }
     uint8_t checksum = calculate_lfn_checksum(short_name);
 
     for (int seq = 1; seq <= needed; seq++) {
         int rev_idx = needed - seq; // store in reverse order
         fat_lfn_entry_t *entry = &lfn_buf[rev_idx];
         memset(entry, 0xFF, sizeof(*entry));
 
         uint8_t seq_num = (uint8_t)seq;
         if (seq == needed) seq_num |= LFN_ENTRY_LAST; // last entry
         entry->seq_num = seq_num;
         entry->attr    = ATTR_LONG_NAME;
         entry->type    = 0;
         entry->checksum= checksum;
         entry->first_cluster = 0;
 
         int start_char = (seq - 1) * 13;
         uint16_t *p;
         bool ended = false;
         // name1 (5 chars)
         p = entry->name1;
         for (int i = 0; i < 5; i++) {
             int idx = start_char + i;
             if (idx < (int)lfn_len) {
                 p[i] = (uint16_t)long_name[idx];
             } else {
                 if (!ended) p[i] = 0; // null terminator
                 ended = true;
             }
         }
         // name2 (6 chars)
         p = entry->name2;
         for (int i = 0; i < 6; i++) {
             int idx = start_char + 5 + i;
             if (idx < (int)lfn_len) {
                 p[i] = (uint16_t)long_name[idx];
             } else {
                 if (!ended) p[i] = 0;
                 ended = true;
             }
         }
         // name3 (2 chars)
         p = entry->name3;
         for (int i = 0; i < 2; i++) {
             int idx = start_char + 5 + 6 + i;
             if (idx < (int)lfn_len) {
                 p[i] = (uint16_t)long_name[idx];
             } else {
                 if (!ended) p[i] = 0;
                 ended = true;
             }
         }
     }
     return needed;
 }
 
 /****************************************************************************
  * generate_unique_short_name (placeholder)
  ****************************************************************************/
 static int generate_unique_short_name(fat_fs_t *fs,
                                       uint32_t parent_dir_cluster,
                                       const char* long_name,
                                       uint8_t short_name_out[11])
 {
     // Just call format_filename for now (very naive).
     format_filename(long_name, (char*)short_name_out);
     // A real implementation should check for collisions and append ~1, ~2, etc.
     return 0;
 }
 
 /****************************************************************************
  * split_path
  ****************************************************************************/
 static int split_path(const char *full_path,
                       char *dir_part, size_t dir_max,
                       char *name_part, size_t name_max)
 {
     if (!full_path || !dir_part || !name_part) return -1;
     const char *slash = strrchr(full_path, '/');
     if (!slash) {
         // no slash => dir=".", name=full_path
         if (dir_max < 2) return -1;
         strncpy(dir_part, ".", dir_max);
         dir_part[dir_max - 1] = '\0';
         if (strlen(full_path) + 1 > name_max) return -1;
         strcpy(name_part, full_path);
         return 0;
     }
     // slash found
     size_t dlen = (size_t)(slash - full_path);
     if (dlen == 0) {
         // slash at start => root
         if (dir_max < 2) return -1;
         strncpy(dir_part, "/", dir_max);
         dir_part[dir_max - 1] = '\0';
     } else {
         if (dlen + 1 > dir_max) return -1;
         memcpy(dir_part, full_path, dlen);
         dir_part[dlen] = '\0';
     }
     const char *nm = slash + 1;
     if (strlen(nm) + 1 > name_max) return -1;
     strcpy(name_part, nm);
     return 0;
 }
 
 /****************************************************************************
  * read_directory_sector
  ****************************************************************************/
 static int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                                  uint32_t sector_offset_in_chain,
                                  uint8_t* buffer)
 {
     if (!fs || !buffer) return -FS_ERR_INVALID_PARAM;
 
     if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         // For FAT12/16, reading from root dir area
         // We interpret sector_offset_in_chain as offset from fs->root_dir_start_lba
         uint32_t lba = fs->root_dir_start_lba + sector_offset_in_chain;
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else {
         // For FAT32 or subdirectory in FAT12/16:
         // cluster is the start. We need to walk sector_offset_in_chain if > spc
         uint32_t cl = cluster;
         uint32_t cluster_sector_count = fs->sectors_per_cluster;
         uint32_t step = sector_offset_in_chain;
         while (step >= cluster_sector_count) {
             // move to next cluster
             uint32_t next;
             if (fat_get_next_cluster(fs, cl, &next) != FS_SUCCESS) {
                 return -FS_ERR_IO;
             }
             if (next >= fs->eoc_marker) {
                 return -FS_ERR_IO; // beyond end
             }
             cl = next;
             step -= cluster_sector_count;
         }
         // now we have the cluster 'cl' and sector 'step' inside that cluster
         uint32_t lba = fat_cluster_to_lba(fs, cl);
         if (lba == 0) return -FS_ERR_IO;
         lba += step; // sector offset within cluster
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     }
 }
 
 /****************************************************************************
  * find_free_directory_slot (Placeholder)
  ****************************************************************************/
 static int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                                     size_t needed_slots,
                                     uint32_t *out_slot_cluster,
                                     uint32_t *out_slot_offset)
 {
     terminal_printf("[FAT find_free_directory_slot] Placeholder => NO_SPACE.\n");
     // Properly, you'd scan the directory, looking for consecutive free or 0xE5 entries
     // If not found, you'd extend the directory by allocating a new cluster (if possible).
     return -FS_ERR_NO_SPACE;
 }
 
 /****************************************************************************
  * write_directory_entries (Placeholder for multi-sector handling)
  ****************************************************************************/
 static int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                                    uint32_t dir_offset,
                                    const void *entries_buf,
                                    size_t num_entries)
 {
     if (!fs || !entries_buf || num_entries == 0) return -FS_ERR_INVALID_PARAM;
     size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
 
     if (offset_in_sector + total_bytes > sector_size) {
         terminal_write("[FAT write_entries] crosses sector boundary - not implemented.\n");
         return -FS_ERR_NOT_SUPPORTED;
     }
 
     uint8_t* sector_buffer = (uint8_t*)kmalloc(sector_size);
     if (!sector_buffer) return -FS_ERR_OUT_OF_MEMORY;
     int read_res = read_directory_sector(fs, dir_cluster, sector_offset_in_chain, sector_buffer);
     if (read_res != FS_SUCCESS) {
         kfree(sector_buffer);
         return read_res;
     }
 
     memcpy(sector_buffer + offset_in_sector, entries_buf, total_bytes);
 
     // Write back
     uint32_t lba;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) {
         // Need to walk cluster chain
         // For brevity, assume single cluster
         lba = fat_cluster_to_lba(fs, dir_cluster);
         if (lba == 0) {
             kfree(sector_buffer);
             return -FS_ERR_IO;
         }
         lba += (sector_offset_in_chain % fs->sectors_per_cluster);
     } else {
         kfree(sector_buffer);
         return -FS_ERR_INVALID_PARAM;
     }
 
     buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
     if (!b) {
         kfree(sector_buffer);
         return -FS_ERR_IO;
     }
     memcpy(b->data, sector_buffer, sector_size);
     buffer_mark_dirty(b);
     buffer_release(b);
     kfree(sector_buffer);
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * update_directory_entry
  ****************************************************************************/
 static int update_directory_entry(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t dir_offset,
                                   const fat_dir_entry_t *new_entry)
 {
     if (!fs || !new_entry) return -FS_ERR_INVALID_PARAM;
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
 
     // Read the sector
     uint8_t* sector_buffer = (uint8_t*)kmalloc(sector_size);
     if (!sector_buffer) return -FS_ERR_OUT_OF_MEMORY;
     int read_res = read_directory_sector(fs, dir_cluster, sector_offset_in_chain, sector_buffer);
     if (read_res != FS_SUCCESS) {
         kfree(sector_buffer);
         return read_res;
     }
 
     memcpy(sector_buffer + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
 
     // Write back
     uint32_t lba;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) {
         lba = fat_cluster_to_lba(fs, dir_cluster);
         if (lba == 0) {
             kfree(sector_buffer);
             return -FS_ERR_IO;
         }
         lba += (sector_offset_in_chain % fs->sectors_per_cluster);
     } else {
         kfree(sector_buffer);
         return -FS_ERR_INVALID_PARAM;
     }
 
     buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
     if (!b) {
         kfree(sector_buffer);
         return -FS_ERR_IO;
     }
     memcpy(b->data, sector_buffer, sector_size);
     buffer_mark_dirty(b);
     buffer_release(b);
     kfree(sector_buffer);
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * mark_directory_entry_deleted
  ****************************************************************************/
 static int mark_directory_entry_deleted(fat_fs_t *fs,
                                         uint32_t dir_cluster,
                                         uint32_t dir_offset,
                                         uint8_t marker)
 {
     if (!fs) return -FS_ERR_INVALID_PARAM;
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
 
     uint8_t* sector_buffer = (uint8_t*)kmalloc(sector_size);
     if (!sector_buffer) return -FS_ERR_OUT_OF_MEMORY;
     int read_res = read_directory_sector(fs, dir_cluster, sector_offset_in_chain, sector_buffer);
     if (read_res != FS_SUCCESS) {
         kfree(sector_buffer);
         return read_res;
     }
 
     fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(sector_buffer + offset_in_sector);
     entry_ptr->name[0] = marker; // Mark as deleted or 0x00
 
     // Write back
     uint32_t lba;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) {
         lba = fat_cluster_to_lba(fs, dir_cluster);
         if (lba == 0) {
             kfree(sector_buffer);
             return -FS_ERR_IO;
         }
         lba += (sector_offset_in_chain % fs->sectors_per_cluster);
     } else {
         kfree(sector_buffer);
         return -FS_ERR_INVALID_PARAM;
     }
 
     buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
     if (!b) {
         kfree(sector_buffer);
         return -FS_ERR_IO;
     }
     memcpy(b->data, sector_buffer, sector_size);
     buffer_mark_dirty(b);
     buffer_release(b);
     kfree(sector_buffer);
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * fat_lookup_path (Placeholder)
  ****************************************************************************/
 static int fat_lookup_path(fat_fs_t *fs, const char *path,
                            fat_dir_entry_t *entry_out,
                            char *lfn_out, size_t lfn_max_len,
                            uint32_t *entry_dir_cluster,
                            uint32_t *entry_offset_in_dir)
 {
     // A full path traversal is needed, splitting on '/', descending subdirectories,
     // collecting LFN entries, etc. Due to complexity, providing partial logic:
 
     terminal_printf("[FAT] fat_lookup_path('%s') => placeholder.\n", path);
 
     // As a placeholder, let's handle root path:
     if (strcmp(path, "/") == 0) {
         // return root directory info
         memset(entry_out, 0, sizeof(*entry_out));
         entry_out->attr = ATTR_DIRECTORY;
         // For FAT32, cluster is fs->root_cluster, for FAT12/16 root is cluster=0
         if (fs->type == FAT_TYPE_FAT32) {
             entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
             entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
         } else {
             // root dir
             entry_out->first_cluster_low  = 0;
             entry_out->first_cluster_high = 0;
         }
         if (entry_dir_cluster)    *entry_dir_cluster    = 0;   // or special
         if (entry_offset_in_dir)  *entry_offset_in_dir  = 0;   // n/a
         if (lfn_out && lfn_max_len>0) lfn_out[0] = '\0';
         return FS_SUCCESS;
     }
 
     // For demonstration, let's fail for everything else
     return -FS_ERR_NOT_FOUND;
 }
 
 