/****************************************************************************
 * fat.c - FAT12/16/32 Filesystem Driver Implementation
 *
 * Features & Fixes:
 *  - Subdirectory traversal with fat_lookup_path (placeholder/partial)
 *  - LFN (Long File Name) reading + basic writing for O_CREAT
 *  - Basic LFN deletion support in unlink
 *  - Cluster allocation/extension for file writes
 *  - O_TRUNC, O_CREAT logic
 *  - fat_unlink_internal (marks entry + LFNs deleted, frees clusters)
 *  - fat_readdir_internal returning struct dirent with partial LFN
 *  - find_free_directory_slot stub (needs extension logic if full)
 *  - write_directory_entries for writing LFN+8.3 sets
 *  - Directory entry updates on close
 *  - FAT12/16 root directory scanning/writing placeholders
 *  - Basic concurrency placeholders with spinlock
 *  - Utility function conflicts resolved: moved to fat_utils.c
 *  - lseek overflow checks with LONG_MAX / LONG_MIN
 ****************************************************************************/

 #include "fat.h"
 #include "terminal.h"       // Logging
 #include "kmalloc.h"        // Memory allocation
 #include "buddy.h"          // Underlying allocator (fallback)
 #include "disk.h"           // Disk operations
 #include "vfs.h"            // VFS structures and registration
 #include "fs_errno.h"       // Filesystem error codes (+ FS_ERR_OVERFLOW)
 #include "fat_utils.h"      // format_filename, plus cluster/FAT helpers now in fat_utils.c
 #include "buffer_cache.h"   // Disk block caching
 #include "sys_file.h"       // O_* flags
 #include "types.h"          // Core types (uint32_t, etc.)
 #include "spinlock.h"       // Concurrency control (placeholder)
 #include <string.h>         // memcpy, memcmp, memset, strchr, strtok, etc.
 #include <libc/stdint.h>    // e.g. uint32_t in some setups
 #include "libc/stdbool.h"
 #include "fs_limits.h"
 #include "libc/limits.h" // For LONG_MAX, LONG_MIN
 
 #ifndef FS_MAX_PATH_LENGTH
 #define FS_MAX_PATH_LENGTH 256
 #endif
 
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
 #ifndef MAX_FILENAME_LEN
 #define MAX_FILENAME_LEN    255
 #endif
 
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
 
 #ifndef FAT_TYPE_FAT12
 #define FAT_TYPE_FAT12 1
 #define FAT_TYPE_FAT16 2
 #define FAT_TYPE_FAT32 3
 #endif
 
 /****************************************************************************
  * Core File Context
  ****************************************************************************/
 /*  NOTE: The 'fat_fs_t' struct is defined in fat.h. We only reference it here. */
 
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
 // VFS ops
 static void    *fat_mount_internal(const char *device);
 static int      fat_unmount_internal(void *fs_context);
 static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
 static int      fat_read_internal(file_t *file, void *buf, size_t len);
 static int      fat_write_internal(file_t *file, const void *buf, size_t len);
 static int      fat_close_internal(file_t *file);
 static off_t    fat_lseek_internal(file_t *file, off_t offset, int whence);
 static int      fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index);
 static int      fat_unlink_internal(void *fs_context, const char *path);
 
 // Lower-level helpers
 static int      read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer);
 static int      write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer);
 static int      load_fat_table(fat_fs_t *fs);
 static int      flush_fat_table(fat_fs_t *fs);
 
 static uint32_t find_free_cluster(fat_fs_t *fs);
 static uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster);
 static int      fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster);
 
 static int      read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                     void *buf, size_t len);
 static int      write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster,
                                      const void *buf, size_t len);
 
 static uint8_t  calculate_lfn_checksum(const uint8_t name_8_3[11]);
 static void     reconstruct_lfn(fat_lfn_entry_t lfn_entries[], int lfn_count,
                                 char *lfn_buf, size_t lfn_buf_size);
 static int      generate_lfn_entries(const char* long_name,
                                      const uint8_t short_name[11],
                                      fat_lfn_entry_t* lfn_buf,
                                      int max_lfn_entries);
 
 static int      generate_unique_short_name(fat_fs_t *fs,
                                            uint32_t parent_dir_cluster,
                                            const char* long_name,
                                            uint8_t short_name_out[11]);
 static int      split_path(const char *full_path,
                            char *dir_part, size_t dir_max,
                            char *name_part, size_t name_max);
 
 static int      find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                                          size_t needed_slots,
                                          uint32_t *out_slot_cluster,
                                          uint32_t *out_slot_offset);
 static int      write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                                         uint32_t dir_offset,
                                         const void *entries_buf,
                                         size_t num_entries);
 static int      update_directory_entry(fat_fs_t *fs,
                                        uint32_t dir_cluster,
                                        uint32_t dir_offset,
                                        const fat_dir_entry_t *new_entry);
 static int      mark_directory_entry_deleted(fat_fs_t *fs,
                                              uint32_t dir_cluster,
                                              uint32_t dir_offset,
                                              uint8_t marker);
 static int      read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                                       uint32_t sector_offset_in_chain,
                                       uint8_t* buffer);
 static int      fat_lookup_path(fat_fs_t *fs, const char *path,
                                 fat_dir_entry_t *entry_out, char *lfn_out,
                                 size_t lfn_max_len,
                                 uint32_t *entry_dir_cluster,
                                 uint32_t *entry_offset_in_dir);
 
 // Comparison helpers
 static int fat_compare_lfn(const char *component, const char *reconstructed_lfn)
 {
     // Placeholder: do a simple case-sensitive compare
     return strcmp(component, reconstructed_lfn);
 }
 
 static int fat_compare_8_3(const char *component, const uint8_t fat_name[11])
 {
     // Convert component to 8.3 then memcmp. Using `format_filename`.
     char formatted[12];
     memset(formatted, 0, sizeof(formatted));
     format_filename(component, formatted);
     return memcmp(formatted, fat_name, 11);
 }
 
 // Helper to get cluster from a directory entry
 static uint32_t fat_get_entry_cluster(const fat_dir_entry_t *e)
 {
     if (!e) return 0;
     return (((uint32_t)e->first_cluster_high) << 16) | e->first_cluster_low;
 }
 
 /****************************************************************************
  * Register / Unregister
  ****************************************************************************/
 int fat_register_driver(void)
 {
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
 
 void fat_unregister_driver(void)
 {
     terminal_write("[FAT] Unregistering FAT driver.\n");
     vfs_unregister_driver(&fat_vfs_driver);
 }
 
 /****************************************************************************
  * MOUNT
  ****************************************************************************/
 static void *fat_mount_internal(const char *device)
 {
     terminal_printf("[FAT] Mounting device '%s'...\n", device ? device : "(null)");
     if (!device) return NULL;
 
     // Allocate our fs struct (fat_fs_t is declared in fat.h)
     fat_fs_t *fs = kmalloc(sizeof(fat_fs_t));
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

// Check signature using the EXISTING bs->data buffer
uint8_t* boot_sector_raw = (uint8_t*)bs->data;
if (boot_sector_raw[510] != 0x55 || boot_sector_raw[511] != 0xAA) {
     terminal_printf("[FAT] Invalid boot sector sig (0xAA55 not found at offset 510) on %s.\n", device);
     buffer_release(bs); // Release buffer before freeing fs
     kfree(fs);
     return NULL;
}
buffer_release(bs);
 
     // Basic geometry
     fs->bytes_per_sector    = fs->boot_sector.bytes_per_sector;
     fs->sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
     if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
         terminal_printf("[FAT] Invalid geometry on %s (0 spc?).\n", device);
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
     {
         uint32_t data_sectors = fs->total_sectors - fs->first_data_sector;
         fs->cluster_count = (fs->sectors_per_cluster > 0)
                           ? data_sectors / fs->sectors_per_cluster
                           : 0;
     }
 
     // Determine FAT type
     if (fs->cluster_count < 4085) {
         fs->type = FAT_TYPE_FAT12;
         fs->root_cluster = 0;    // not used for FAT12
         fs->eoc_marker   = 0xFF8; // typical FAT12 EOC range 0xFF8..0xFFF
         terminal_write("[FAT] Detected FAT12.\n");
     }
     else if (fs->cluster_count < 65525) {
         fs->type = FAT_TYPE_FAT16;
         fs->root_cluster = 0;
         fs->eoc_marker   = 0xFFF8; // typical FAT16 EOC range 0xFFF8..0xFFFF
         terminal_write("[FAT] Detected FAT16.\n");
     }
     else {
         fs->type = FAT_TYPE_FAT32;
         fs->root_cluster = fs->boot_sector.root_cluster;
         fs->eoc_marker   = 0x0FFFFFF8; // typical FAT32 EOC range
         terminal_write("[FAT] Detected FAT32.\n");
     }
 
     // Load the FAT
     if (load_fat_table(fs) != FS_SUCCESS) {
         terminal_printf("[FAT] Failed to load FAT for %s.\n", device);
         kfree(fs);
         return NULL;
     }
 
     terminal_printf("[FAT] Mounted '%s' as FAT.\n", device);
     return fs;
 }
 
 /****************************************************************************
  * UNMOUNT
  ****************************************************************************/
 static int fat_unmount_internal(void *fs_context)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs) return -FS_ERR_INVALID_PARAM;
 
     terminal_printf("[FAT] Unmounting %s (FAT)...\n",
         fs->disk.blk_dev.device_name ? fs->disk.blk_dev.device_name : "(null)");
 
     if (fs->fat_table) {
         flush_fat_table(fs);
         kfree(fs->fat_table);
         fs->fat_table = NULL;
     }
     buffer_cache_sync();
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
     uint32_t entry_dir_cluster = 0, entry_offset_in_dir = 0;
     int find_res = fat_lookup_path(fs, path,
                                    &entry, lfn_buffer, sizeof(lfn_buffer),
                                    &entry_dir_cluster, &entry_offset_in_dir);
 
     bool exists = (find_res == FS_SUCCESS);
     bool created = false, truncated = false;
 
     vnode_t *vnode = NULL;
     fat_file_context_t *file_ctx = NULL;
     int ret_err = FS_SUCCESS;
 
     // If not exists and O_CREAT => create
     if (!exists && (flags & O_CREAT)) {
         created = true;
         char parent_dir_path[FS_MAX_PATH_LENGTH];
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
 
         uint32_t p_clus = fat_get_entry_cluster(&parent_entry);
         if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_dir_path, "/") == 0) {
             // FAT12/16 root is cluster=0
             p_clus = 0;
         }
 
         // Generate short name
         uint8_t short_name[11];
         if (generate_unique_short_name(fs, p_clus, new_name, short_name) != 0) {
             ret_err = -FS_ERR_UNKNOWN;
             goto open_fail_locked;
         }
 
         // LFN entries
         fat_lfn_entry_t lfn_entries[MAX_LFN_ENTRIES];
         int lfn_count = generate_lfn_entries(new_name, short_name, lfn_entries, MAX_LFN_ENTRIES);
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
                                         lfn_entries, lfn_count) != FS_SUCCESS)
             {
                 ret_err = -FS_ERR_IO;
                 goto open_fail_locked;
             }
             cur_off += (uint32_t)(lfn_count * sizeof(fat_dir_entry_t));
         }
         // final 8.3
         if (write_directory_entries(fs, slot_cluster, cur_off,
                                     &entry, 1) != FS_SUCCESS)
         {
             ret_err = -FS_ERR_IO;
             goto open_fail_locked;
         }
 
         entry_dir_cluster   = slot_cluster;
         entry_offset_in_dir = cur_off;
         buffer_cache_sync();
         terminal_printf("[FAT O_CREAT] Created '%s'\n", path);
         exists = true;
     }
     else if (!exists) {
         // file doesn't exist, no O_CREAT
         ret_err = find_res;
         goto open_fail_locked;
     }
 
     // Now 'entry' is the file/dir. Check permissions
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
     vnode = kmalloc(sizeof(vnode_t));
     file_ctx = kmalloc(sizeof(fat_file_context_t));
     if (!vnode || !file_ctx) {
         ret_err = -FS_ERR_OUT_OF_MEMORY;
         goto open_fail_locked;
     }
     memset(vnode, 0, sizeof(*vnode));
     memset(file_ctx, 0, sizeof(*file_ctx));
 
     // Fill the context
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
     terminal_printf("[FAT open] Failed for path '%s'. Err=%d\n", path, ret_err);
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
     if (fctx->is_directory) return -FS_ERR_IS_A_DIRECTORY;
     fat_fs_t *fs = fctx->fs;
 
     // Lock for file_size check
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     if (file->offset < 0) file->offset = 0;
     if ((uint64_t)file->offset >= fctx->file_size) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return 0;
     }
 
     size_t remain = fctx->file_size - (size_t)file->offset;
     if (len > remain) len = remain;
     if (len == 0) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return 0;
     }
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_INVALID_FORMAT;
     }
 
     size_t total_read = 0;
     size_t bytes_left = len;
     off_t  user_offset = file->offset;
 
     uint32_t cluster_index = (uint32_t)(user_offset / cluster_size);
     uint32_t offset_in_cluster = (uint32_t)(user_offset % cluster_size);
 
     uint32_t current_cluster = fctx->first_cluster;
     if (current_cluster < 2 && fctx->file_size > 0) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_CORRUPT;
     }
     if (fctx->first_cluster < 2 && fctx->file_size == 0) {
         // Empty file
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return 0;
     }
 
     // Step to cluster_index
     for (uint32_t i = 0; i < cluster_index; i++) {
         uint32_t next;
         if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             return -FS_ERR_IO;
         }
         if (next >= fs->eoc_marker) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             return -FS_ERR_CORRUPT;
         }
         current_cluster = next;
     }
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // Read in a loop
     while (bytes_left > 0 && current_cluster >= 2 && current_cluster < fs->eoc_marker) {
         size_t to_read_in_cluster = cluster_size - offset_in_cluster;
         if (to_read_in_cluster > bytes_left) {
             to_read_in_cluster = bytes_left;
         }
 
         int rc = read_cluster_cached(fs, current_cluster, offset_in_cluster,
                                      (uint8_t*)buf + total_read,
                                      to_read_in_cluster);
         if (rc < 0) {
             return rc;
         }
         if ((size_t)rc != to_read_in_cluster) {
             file->offset += rc;
             return total_read + rc;
         }
 
         total_read     += to_read_in_cluster;
         bytes_left     -= to_read_in_cluster;
         user_offset    += to_read_in_cluster;
         offset_in_cluster = 0;
 
         if (bytes_left > 0) {
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             uint32_t next;
             if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
                 spinlock_release_irqrestore(&fs->lock, irq_flags);
                 file->offset += total_read;
                 return -FS_ERR_IO;
             }
             spinlock_release_irqrestore(&fs->lock, irq_flags);
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
 
     // O_APPEND
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     if (file->flags & O_APPEND) {
         file->offset = (off_t)fctx->file_size;
     }
     if (file->offset < 0) file->offset = 0;
 
     if (fctx->first_cluster < 2 && fctx->file_size == 0) {
         // Allocate first cluster
         uint32_t newc = fat_allocate_cluster(fs, 0);
         if (newc < 2) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             return -FS_ERR_NO_SPACE;
         }
         fctx->first_cluster   = newc;
         fctx->current_cluster = newc;
         fctx->dirty = true;
     }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     size_t total_written = 0;
     size_t bytes_left = len;
     off_t  user_offset = file->offset;
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     while (bytes_left > 0) {
         uint32_t cluster_index = (uint32_t)(user_offset / cluster_size);
         uint32_t offset_in_cluster = (uint32_t)(user_offset % cluster_size);
 
         // Walk to cluster_index
         uint32_t current_cluster = fctx->first_cluster;
         if (current_cluster < 2) return -FS_ERR_CORRUPT;
 
         for (uint32_t i = 0; i < cluster_index; i++) {
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             uint32_t next;
             if (fat_get_next_cluster(fs, current_cluster, &next) != FS_SUCCESS) {
                 spinlock_release_irqrestore(&fs->lock, irq_flags);
                 return -FS_ERR_IO;
             }
             if (next >= fs->eoc_marker) {
                 next = fat_allocate_cluster(fs, current_cluster);
                 if (next < 2) {
                     spinlock_release_irqrestore(&fs->lock, irq_flags);
                     // Update file size if partial
                     if ((size_t)user_offset > fctx->file_size) {
                         fctx->file_size = (size_t)user_offset;
                     }
                     fctx->dirty = true;
                     return -FS_ERR_NO_SPACE;
                 }
             }
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             current_cluster = next;
         }
 
         size_t to_write_in_cluster = cluster_size - offset_in_cluster;
         if (to_write_in_cluster > bytes_left) {
             to_write_in_cluster = bytes_left;
         }
 
         // partial => read-modify-write
         if (offset_in_cluster != 0 || to_write_in_cluster < cluster_size) {
             uint8_t bounce[4096];
             if (cluster_size > sizeof(bounce)) {
                 return -FS_ERR_NOT_SUPPORTED;
             }
             int rc = read_cluster_cached(fs, current_cluster, 0, bounce, cluster_size);
             if (rc < 0) return rc;
             memcpy(bounce + offset_in_cluster,
                    (uint8_t*)buf + total_written,
                    to_write_in_cluster);
 
             rc = write_cluster_cached(fs, current_cluster, 0, bounce, cluster_size);
             if (rc < 0) return rc;
             if ((size_t)rc != cluster_size) {
                 file->offset += (rc - offset_in_cluster);
                 irq_flags = spinlock_acquire_irqsave(&fs->lock);
                 if (file->offset > (off_t)fctx->file_size) {
                     fctx->file_size = (size_t)file->offset;
                 }
                 fctx->dirty = true;
                 spinlock_release_irqrestore(&fs->lock, irq_flags);
                 return (int)(total_written + (rc - offset_in_cluster));
             }
         }
         else {
             // full cluster
             int rc = write_cluster_cached(fs, current_cluster, 0,
                                           (uint8_t*)buf + total_written,
                                           cluster_size);
             if (rc < 0) return rc;
             if ((size_t)rc != cluster_size) {
                 file->offset += rc;
                 irq_flags = spinlock_acquire_irqsave(&fs->lock);
                 if (file->offset > (off_t)fctx->file_size) {
                     fctx->file_size = (size_t)file->offset;
                 }
                 fctx->dirty = true;
                 spinlock_release_irqrestore(&fs->lock, irq_flags);
                 return total_written + rc;
             }
         }
 
         total_written += to_write_in_cluster;
         bytes_left    -= to_write_in_cluster;
         user_offset   += to_write_in_cluster;
 
         // update file_size if extended
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         if ((size_t)user_offset > fctx->file_size) {
             fctx->file_size = (size_t)user_offset;
         }
         fctx->dirty = true;
         spinlock_release_irqrestore(&fs->lock, irq_flags);
     }
 
     file->offset = user_offset;
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
     fat_fs_t *fs = fctx->fs;
     if (!fs) return (off_t)-FS_ERR_INVALID_PARAM;
 
     off_t new_off;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     uint32_t file_size = fctx->file_size;
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     switch (whence) {
     case SEEK_SET:
         new_off = offset;
         break;
     case SEEK_CUR: {
         // Check for overflow using LONG_MAX/MIN
         long cur = (long)file->offset;
         long off = (long)offset;
         if ((off > 0 && cur > LONG_MAX - off) ||
             (off < 0 && cur < LONG_MIN - off)) {
             return (off_t)-FS_ERR_OVERFLOW;
         }
         new_off = cur + off;
         break;
     }
     case SEEK_END: {
         long fsz = (long)file_size;
         long off = (long)offset;
         if ((off > 0 && fsz > LONG_MAX - off) ||
             (off < 0 && fsz < LONG_MIN - off)) {
             return (off_t)-FS_ERR_OVERFLOW;
         }
         new_off = fsz + off;
         break;
     }
     default:
         return (off_t)-FS_ERR_INVALID_PARAM;
     }
 
     if (new_off < 0) {
         return (off_t)-FS_ERR_INVALID_PARAM;
     }
 
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
         fat_dir_entry_t old_entry;
         bool read_ok = false;
         size_t sec_size = fs->bytes_per_sector;
         if (sec_size > 0) {
             uint32_t sector_offset_in_chain = fctx->dir_entry_offset / sec_size;
             size_t offset_in_sector = fctx->dir_entry_offset % sec_size;
 
             uint8_t* sector_buffer = kmalloc(sec_size);
             if (sector_buffer) {
                 if (read_directory_sector(fs, fctx->dir_entry_cluster,
                                           sector_offset_in_chain,
                                           sector_buffer) == FS_SUCCESS)
                 {
                     memcpy(&old_entry, sector_buffer + offset_in_sector, sizeof(fat_dir_entry_t));
                     read_ok = true;
                 }
                 kfree(sector_buffer);
             }
         }
         if (read_ok) {
             old_entry.file_size = fctx->file_size;
             old_entry.first_cluster_low  = (uint16_t)(fctx->first_cluster & 0xFFFF);
             old_entry.first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
             if (update_directory_entry(fs, fctx->dir_entry_cluster,
                                        fctx->dir_entry_offset, &old_entry) == FS_SUCCESS)
             {
                 flush_fat_table(fs);
                 buffer_cache_sync();
             }
         }
     }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // Free context
     kfree(file->vnode->data);
     file->vnode->data = NULL;
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
 
     uint32_t fc = fat_get_entry_cluster(&entry);
     if (fc >= 2) {
         fat_free_cluster_chain(fs, fc);
     }
 
     // Mark 8.3 entry deleted. (Skipping LFN backward scan for brevity)
     mark_directory_entry_deleted(fs, dir_cluster, entry_offset, DIR_ENTRY_DELETED);
 
     flush_fat_table(fs);
     buffer_cache_sync();
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     terminal_printf("[FAT unlink] Unlinked '%s'.\n", path);
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
 
     if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
         fctx->readdir_current_cluster = fctx->first_cluster;
         if (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0) {
             fctx->readdir_current_cluster = 0;
         }
         fctx->readdir_current_offset = 0;
         fctx->readdir_last_index = (size_t)-1;
     }
 
     bool searching_fixed_root = (fctx->readdir_current_cluster == 0 && fs->type != FAT_TYPE_FAT32);
     uint8_t *buffer = kmalloc(fs->bytes_per_sector);
     if (!buffer) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     fat_lfn_entry_t lfn_collector[MAX_LFN_ENTRIES];
     int lfn_count = 0;
     size_t current_entry_idx_scan = fctx->readdir_last_index + 1;
     int ret = -FS_ERR_NOT_FOUND;
 
     while (true) {
         uint32_t sec_size = fs->bytes_per_sector;
         uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
         size_t offset_in_sector = fctx->readdir_current_offset % sec_size;
         size_t entries_per_sector = sec_size / sizeof(fat_dir_entry_t);
 
         int rds = read_directory_sector(fs, fctx->readdir_current_cluster,
                                         sector_offset_in_chain, buffer);
         if (rds != FS_SUCCESS) {
             ret = (rds == -FS_ERR_IO) ? -FS_ERR_IO : -FS_ERR_NOT_FOUND;
             break;
         }
 
         for (size_t e_i = offset_in_sector / sizeof(fat_dir_entry_t);
              e_i < entries_per_sector; e_i++)
         {
             fat_dir_entry_t *dent = (fat_dir_entry_t*)(buffer + e_i * sizeof(fat_dir_entry_t));
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
 
             if (dent->name[0] == DIR_ENTRY_UNUSED) {
                 ret = -FS_ERR_NOT_FOUND;
                 goto done_readdir;
             }
             if (dent->name[0] == DIR_ENTRY_DELETED) {
                 lfn_count = 0;
                 continue;
             }
             if ((dent->attr & ATTR_VOLUME_ID) && !(dent->attr & ATTR_LONG_NAME)) {
                 lfn_count = 0;
                 continue;
             }
 
             if ((dent->attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
                 // LFN
                 fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                 if (lfn_count < MAX_LFN_ENTRIES) {
                     lfn_collector[lfn_count++] = *lfn_ent;
                 } else {
                     lfn_count = 0; // overflow
                 }
                 continue;
             }
 
             // 8.3 entry
             if (current_entry_idx_scan == entry_index) {
                 char tmp_name[MAX_LFN_CHARS];
                 tmp_name[0] = '\0';
                 if (lfn_count > 0) {
                     uint8_t expected_sum = calculate_lfn_checksum(dent->name);
                     if (lfn_collector[0].checksum == expected_sum) {
                         reconstruct_lfn(lfn_collector, lfn_count, tmp_name, sizeof(tmp_name));
                     }
                 }
                 if (tmp_name[0] == '\0') {
                     memcpy(tmp_name, dent->name, 11);
                     tmp_name[11] = '\0';
                 }
 
                 strncpy(d_entry_out->d_name, tmp_name, sizeof(d_entry_out->d_name) - 1);
                 d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0';
 
                 uint32_t fc = (((uint32_t)dent->first_cluster_high) << 16) | dent->first_cluster_low;
                 d_entry_out->d_ino = fc;
                 d_entry_out->d_type = (dent->attr & ATTR_DIRECTORY) ? DT_DIR : DT_REG;
 
                 fctx->readdir_last_index = entry_index;
                 ret = FS_SUCCESS;
                 goto done_readdir;
             }
 
             current_entry_idx_scan++;
             lfn_count = 0;
         }
 
         if (!searching_fixed_root && (fctx->readdir_current_offset % fs->cluster_size_bytes == 0)) {
             uint32_t next_c;
             if (fat_get_next_cluster(fs, fctx->readdir_current_cluster, &next_c) != FS_SUCCESS) {
                 ret = -FS_ERR_IO;
                 break;
             }
             if (next_c >= fs->eoc_marker) {
                 ret = -FS_ERR_NOT_FOUND;
                 break;
             }
             fctx->readdir_current_cluster = next_c;
             fctx->readdir_current_offset  = 0;
         }
         else if (searching_fixed_root) {
             if (sector_offset_in_chain + 1 >= fs->root_dir_sectors) {
                 ret = -FS_ERR_NOT_FOUND;
                 break;
             }
         }
     }
 
 done_readdir:
     kfree(buffer);
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return ret;
 }
 
 /****************************************************************************
  * Internal Helper Implementations
  ****************************************************************************/
 static int read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer)
 {
     if (!fs || !buffer) return -FS_ERR_INVALID_PARAM;
     uint32_t target_lba = fs->fat_start_lba + sector_offset;
     if (fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
 
     buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
     if (!buf) return -FS_ERR_IO;
     memcpy(buffer, buf->data, fs->bytes_per_sector);
     buffer_release(buf);
     return FS_SUCCESS;
 }
 
 static int write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer)
 {
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
 
 static int load_fat_table(fat_fs_t *fs)
 {
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
     terminal_printf("[FAT] FAT table loaded (%u sectors).\n", fs->fat_size);
     return FS_SUCCESS;
 }
 
 static int flush_fat_table(fat_fs_t *fs)
 {
     if (!fs || !fs->fat_table) return FS_SUCCESS; // Nothing to flush
     if (fs->fat_size == 0 || fs->bytes_per_sector == 0) return -FS_ERR_INVALID_FORMAT;
 
     const uint8_t *current_ptr = (const uint8_t*)fs->fat_table;
     for (uint32_t i = 0; i < fs->fat_size; i++) {
         if (write_fat_sector(fs, i, current_ptr) != FS_SUCCESS) {
             return -FS_ERR_IO;
         }
         current_ptr += fs->bytes_per_sector;
     }
     return FS_SUCCESS;
 }
 
 /****************************************************************************
  * find_free_cluster, fat_allocate_cluster, fat_free_cluster_chain
  ****************************************************************************/
 static uint32_t find_free_cluster(fat_fs_t *fs)
 {
     if (!fs || !fs->fat_table) return 0;
     uint32_t total_clusters = fs->cluster_count + 2; // cluster range: 2..(count+1)
 
     if (fs->type == FAT_TYPE_FAT32) {
         uint32_t *FAT32 = (uint32_t*)fs->fat_table;
         for (uint32_t i = 2; i < total_clusters; i++) {
             if ((FAT32[i] & 0x0FFFFFFF) == 0) {
                 return i;
             }
         }
     }
     else if (fs->type == FAT_TYPE_FAT16) {
         uint16_t *FAT16 = (uint16_t*)fs->fat_table;
         for (uint32_t i = 2; i < total_clusters; i++) {
             if (FAT16[i] == 0) {
                 return i;
             }
         }
     }
     else {
         // partial FAT12 or unknown
         return 0;
     }
     return 0;
 }
 
 static uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster)
 {
     if (!fs || !fs->fat_table) return 0;
     uint32_t free_cluster = find_free_cluster(fs);
     if (free_cluster < 2) {
         return 0; // no space
     }
     // Mark new cluster as EOC
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
             // continue freeing anyway
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
 static int read_cluster_cached(fat_fs_t *fs, uint32_t cluster,
                                uint32_t offset_in_cluster,
                                void *buf, size_t len)
 {
     if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;
     if (offset_in_cluster + len > fs->cluster_size_bytes) {
         return -FS_ERR_INVALID_PARAM;
     }
     uint32_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
     uint32_t start_sector = offset_in_cluster / sector_size;
     uint32_t end_sector   = (offset_in_cluster + len - 1) / sector_size;
 
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) return -FS_ERR_IO;
 
     size_t read_offset_in_buf = 0;
     for (uint32_t sec = start_sector; sec <= end_sector; sec++) {
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, cluster_lba + sec);
         if (!b) return -FS_ERR_IO;
 
         size_t offset_in_sec = 0;
         if (sec == start_sector) offset_in_sec = offset_in_cluster % sector_size;
         size_t bytes_this_sector = sector_size - offset_in_sec;
         if (bytes_this_sector > (len - read_offset_in_buf)) {
             bytes_this_sector = (len - read_offset_in_buf);
         }
 
         memcpy((uint8_t*)buf + read_offset_in_buf,
                b->data + offset_in_sec,
                bytes_this_sector);
         read_offset_in_buf += bytes_this_sector;
 
         buffer_release(b);
     }
     return (int)len;
 }
 
 static int write_cluster_cached(fat_fs_t *fs, uint32_t cluster,
                                 uint32_t offset_in_cluster,
                                 const void *buf, size_t len)
 {
     if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;
     if (offset_in_cluster + len > fs->cluster_size_bytes) {
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
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, cluster_lba + sec);
         if (!b) return -FS_ERR_IO;
 
         size_t offset_in_sec = 0;
         if (sec == start_sector) offset_in_sec = offset_in_cluster % sector_size;
         size_t bytes_this_sector = sector_size - offset_in_sec;
         if (bytes_this_sector > (len - written_offset_in_buf)) {
             bytes_this_sector = (len - written_offset_in_buf);
         }
 
         memcpy(b->data + offset_in_sec,
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
 static uint8_t calculate_lfn_checksum(const uint8_t name_8_3[11])
 {
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
     // LFN entries on disk appear in reverse sequence order
     for (int i = lfn_count - 1; i >= 0; i--) {
         uint16_t *parts[] = { lfn_entries[i].name1,
                               lfn_entries[i].name2,
                               lfn_entries[i].name3 };
         size_t counts[] = {5, 6, 2};
         bool done = false;
         for (int p = 0; p < 3 && !done; p++) {
             for (size_t c = 0; c < counts[p]; c++) {
                 uint16_t wc = parts[p][c];
                 if (wc == 0x0000) {
                     done = true;
                     break;
                 }
                 if (wc == 0xFFFF) {
                     continue; 
                 }
                 if (buf_idx < (int)lfn_buf_size - 1) {
                     char ascii = (wc < 128) ? (char)wc : '?';
                     lfn_buf[buf_idx++] = ascii;
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
     size_t lfn_len = strlen(long_name);
     int needed = (int)((lfn_len + 12) / 13);
     if (needed > max_lfn_entries) {
         return 0; // cannot store that many
     }
 
     uint8_t checksum = calculate_lfn_checksum(short_name);
     int total_entries = 0;
 
     for (int seq = 1; seq <= needed; seq++) {
         int rev_idx = needed - seq;
         fat_lfn_entry_t *entry = &lfn_buf[rev_idx];
         memset(entry, 0xFF, sizeof(*entry));
 
         uint8_t seq_num = (uint8_t)seq;
         if (seq == needed) seq_num |= LFN_ENTRY_LAST;
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
                 if (!ended) p[i] = 0;
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
 
         total_entries++;
     }
     return total_entries;
 }
 
 /****************************************************************************
  * generate_unique_short_name (placeholder)
  ****************************************************************************/
 static int generate_unique_short_name(fat_fs_t *fs,
                                       uint32_t parent_dir_cluster,
                                       const char* long_name,
                                       uint8_t short_name_out[11])
 {
     (void)fs; (void)parent_dir_cluster; // not used in this stub
     format_filename(long_name, (char*)short_name_out);
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
         // No slash => all is filename
         if (dir_max < 2) return -1;
         strncpy(dir_part, ".", dir_max);
         dir_part[dir_max - 1] = '\0';
         if (strlen(full_path) + 1 > name_max) return -1;
         strcpy(name_part, full_path);
         return 0;
     }
 
     size_t dlen = (size_t)(slash - full_path);
     if (dlen == 0) {
         // Path starts with '/'
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
  * find_free_directory_slot (Placeholder)
  ****************************************************************************/
 static int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                                     size_t needed_slots,
                                     uint32_t *out_slot_cluster,
                                     uint32_t *out_slot_offset)
 {
     (void)fs; (void)parent_dir_cluster; (void)needed_slots;
     (void)out_slot_cluster; (void)out_slot_offset;
     terminal_printf("[FAT find_free_directory_slot] Placeholder => NO_SPACE.\n");
     return -FS_ERR_NO_SPACE;
 }
 
 /****************************************************************************
  * write_directory_entries (Placeholder)
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
 
     // Simplistic check: writing must fit in one sector (placeholder)
     if (offset_in_sector + total_bytes > sector_size) {
         terminal_write("[FAT write_entries] crosses sector boundary - not implemented.\n");
         return -FS_ERR_NOT_SUPPORTED;
     }
 
     uint8_t* sector_buffer = kmalloc(sector_size);
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
 
     uint8_t* sector_buffer = kmalloc(sector_size);
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
 
     uint8_t* sector_buffer = kmalloc(sector_size);
     if (!sector_buffer) return -FS_ERR_OUT_OF_MEMORY;
 
     int read_res = read_directory_sector(fs, dir_cluster, sector_offset_in_chain, sector_buffer);
     if (read_res != FS_SUCCESS) {
         kfree(sector_buffer);
         return read_res;
     }
 
     fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(sector_buffer + offset_in_sector);
     entry_ptr->name[0] = marker; // 0xE5 or 0x00
 
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
  * read_directory_sector
  ****************************************************************************/
 static int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                                  uint32_t sector_offset_in_chain,
                                  uint8_t* buffer)
 {
     if (!fs || !buffer) return -FS_ERR_INVALID_PARAM;
 
     if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         // FAT12/16 root directory
         uint32_t lba = fs->root_dir_start_lba + sector_offset_in_chain;
         if (lba >= fs->root_dir_start_lba + fs->root_dir_sectors) {
             return -FS_ERR_NOT_FOUND;
         }
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else {
         uint32_t cl = cluster;
         uint32_t cluster_sector_count = fs->sectors_per_cluster;
         uint32_t step = sector_offset_in_chain;
         while (step >= cluster_sector_count) {
             uint32_t next;
             if (fat_get_next_cluster(fs, cl, &next) != FS_SUCCESS) {
                 return -FS_ERR_IO;
             }
             if (next >= fs->eoc_marker) {
                 return -FS_ERR_IO;
             }
             cl = next;
             step -= cluster_sector_count;
         }
         uint32_t lba = fat_cluster_to_lba(fs, cl);
         if (lba == 0) return -FS_ERR_IO;
         lba += step;
         buffer_t* b = buffer_get(fs->disk.blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     }
 }
 
 /****************************************************************************
  * fat_lookup_path
  ****************************************************************************/
 static int fat_lookup_path(fat_fs_t *fs, const char *path,
                            fat_dir_entry_t *entry_out, char *lfn_out,
                            size_t lfn_max_len,
                            uint32_t *entry_dir_cluster_out,
                            uint32_t *entry_offset_in_dir_out)
 {
     if (!fs || !path || !entry_out || !entry_dir_cluster_out || !entry_offset_in_dir_out) {
         return -FS_ERR_INVALID_PARAM;
     }
     if (path[0] != '/') {
         return -FS_ERR_INVALID_PARAM; // Only absolute paths
     }
     if (lfn_out && lfn_max_len > 0) {
         lfn_out[0] = '\0';
     }
 
     // Root check
     if (strcmp(path, "/") == 0) {
         memset(entry_out, 0, sizeof(*entry_out));
         entry_out->attr = ATTR_DIRECTORY;
         if (fs->type == FAT_TYPE_FAT32) {
             entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
             entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
             *entry_dir_cluster_out = 0;
         } else {
             entry_out->first_cluster_low  = 0;
             entry_out->first_cluster_high = 0;
             *entry_dir_cluster_out = 0;
         }
         *entry_offset_in_dir_out = 0;
         if (lfn_out && lfn_max_len > 0) {
             strncpy(lfn_out, "/", lfn_max_len - 1);
             lfn_out[lfn_max_len - 1] = '\0';
         }
         return FS_SUCCESS;
     }
 
     // Tokenize
     char *path_copy = kmalloc(strlen(path) + 1);
     if (!path_copy) return -FS_ERR_OUT_OF_MEMORY;
     strcpy(path_copy, path);
 
     char *tokens[128];
     int token_count = 0;
     char *tok = strtok(path_copy + 1, "/");
     while (tok && token_count < 128) {
         if (strlen(tok) > 0) tokens[token_count++] = tok;
         tok = strtok(NULL, "/");
     }
 
     if (token_count == 0) {
         kfree(path_copy);
         return FS_SUCCESS; // path was just "/"
     }
 
     uint32_t current_dir_cluster;
     bool scanning_fixed_root = false;
     if (fs->type == FAT_TYPE_FAT32) {
         current_dir_cluster = fs->root_cluster;
     } else {
         current_dir_cluster = 0;
         scanning_fixed_root = true;
     }
 
     fat_dir_entry_t current_entry;
     memcpy(&current_entry, entry_out, sizeof(current_entry)); // just init
 
     for (int i = 0; i < token_count; i++) {
         const char *component = tokens[i];
         bool found = false;
 
         uint32_t cluster_scan = current_dir_cluster;
         uint32_t current_byte_offset = 0;
         uint32_t current_sector_offset_in_chain = 0;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         fat_lfn_entry_t lfn_buf_local[MAX_LFN_ENTRIES];
         int lfn_count = 0;
 
         uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
         if (!sector_data) {
             kfree(path_copy);
             return -FS_ERR_OUT_OF_MEMORY;
         }
 
         while (true) {
             if (read_directory_sector(fs, cluster_scan, current_sector_offset_in_chain,
                                       sector_data) != FS_SUCCESS)
             {
                 kfree(sector_data);
                 kfree(path_copy);
                 return -FS_ERR_NOT_FOUND;
             }
 
             for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
                 fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
                 uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
                 if (de->name[0] == DIR_ENTRY_UNUSED) {
                     kfree(sector_data);
                     kfree(path_copy);
                     return -FS_ERR_NOT_FOUND;
                 }
                 if (de->name[0] == DIR_ENTRY_DELETED) {
                     lfn_count = 0;
                     continue;
                 }
                 if ((de->attr & ATTR_VOLUME_ID) && !(de->attr & ATTR_LONG_NAME)) {
                     lfn_count = 0;
                     continue;
                 }
 
                 if ((de->attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {
                     if (lfn_count < MAX_LFN_ENTRIES) {
                         lfn_buf_local[lfn_count++] = *((fat_lfn_entry_t*)de);
                     }
                     continue;
                 }
 
                 // Real 8.3 entry
                 bool match = false;
                 char reconstructed_lfn_buf[MAX_LFN_CHARS];
 
                 if (lfn_count > 0) {
                     uint8_t expected_sum = calculate_lfn_checksum(de->name);
                     if (lfn_buf_local[0].checksum == expected_sum) {
                         reconstruct_lfn(lfn_buf_local, lfn_count,
                                         reconstructed_lfn_buf, sizeof(reconstructed_lfn_buf));
                         if (fat_compare_lfn(component, reconstructed_lfn_buf) == 0) {
                             match = true;
                         }
                     }
                 }
                 if (!match) {
                     if (fat_compare_8_3(component, de->name) == 0) {
                         match = true;
                         if (lfn_out) {
                             lfn_out[0] = '\0'; // no LFN
                         }
                     }
                 }
 
                 if (match) {
                     memcpy(&current_entry, de, sizeof(current_entry));
                     found = true;
 
                     if (i == token_count - 1) {
                         memcpy(entry_out, &current_entry, sizeof(*entry_out));
                         *entry_dir_cluster_out   = cluster_scan;
                         *entry_offset_in_dir_out = entry_abs_offset;
 
                         if (lfn_count > 0 && lfn_out && lfn_max_len > 0) {
                             reconstruct_lfn(lfn_buf_local, lfn_count, lfn_out, lfn_max_len);
                         }
                         kfree(sector_data);
                         kfree(path_copy);
                         return FS_SUCCESS;
                     } else {
                         // not last => must be directory
                         if (!(current_entry.attr & ATTR_DIRECTORY)) {
                             kfree(sector_data);
                             kfree(path_copy);
                             return -FS_ERR_NOT_A_DIRECTORY;
                         }
                         current_dir_cluster =
                             (((uint32_t)current_entry.first_cluster_high) << 16)
                             | current_entry.first_cluster_low;
                         scanning_fixed_root = false;
                         if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0) {
                             scanning_fixed_root = true;
                         }
                         goto next_component;
                     }
                 }
 
                 lfn_count = 0;
             }
 
             current_byte_offset += fs->bytes_per_sector;
             current_sector_offset_in_chain++;
 
             if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
                 uint32_t next;
                 if (fat_get_next_cluster(fs, cluster_scan, &next) != FS_SUCCESS) {
                     kfree(sector_data);
                     kfree(path_copy);
                     return -FS_ERR_IO;
                 }
                 if (next >= fs->eoc_marker) {
                     kfree(sector_data);
                     kfree(path_copy);
                     return -FS_ERR_NOT_FOUND;
                 }
                 cluster_scan = next;
                 current_byte_offset = 0;
                 current_sector_offset_in_chain = 0;
             } else if (scanning_fixed_root) {
                 if (current_sector_offset_in_chain >= fs->root_dir_sectors) {
                     kfree(sector_data);
                     kfree(path_copy);
                     return -FS_ERR_NOT_FOUND;
                 }
             }
         }
 
 next_component:
         if (!found) {
             kfree(sector_data);
             kfree(path_copy);
             return -FS_ERR_NOT_FOUND;
         }
         kfree(sector_data);
     }
 
     kfree(path_copy);
     return -FS_ERR_INTERNAL;
 }
 