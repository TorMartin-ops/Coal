/**
 * @file fat_dir.c
 * @brief Directory operations implementation for FAT filesystem driver.
 *
 * Handles VFS operations like open, readdir, unlink, and the core path
 * resolution logic (lookup). Includes helpers for managing directory entries.
 */

 #include "fat_dir.h"    // Our declarations
 #include "fat_core.h"   // Core structures
 #include "fat_alloc.h"  // Cluster allocation (fat_allocate_cluster, fat_free_cluster_chain)
 #include "fs_util.h"
 #include "fat_utils.h"  // FAT entry access, LBA conversion, name formatting etc.
 #include "fat_lfn.h"    // LFN specific helpers (checksum, reconstruct, generate)
 #include "fat_io.h"     // read_cluster_cached, write_cluster_cached (though less used directly here)
 #include "buffer_cache.h" // Buffer cache access (buffer_get, buffer_release, etc.)
 #include "spinlock.h"   // Locking primitives
 #include "terminal.h"   // Logging
 #include "sys_file.h"   // O_* flags definitions
 #include "kmalloc.h"    // Kernel memory allocation
 #include "fs_errno.h"   // Filesystem error codes
 #include "fs_config.h"  // Filesystem limits (FS_MAX_PATH_LENGTH) - Added Include
 #include <string.h>     // memcpy, memcmp, memset, strlen, strchr, strrchr, strtok
 #include "assert.h"     // KERNEL_ASSERT
 #include "types.h"      // For struct dirent definition - Added Include (may be implicit via others)
 
 // --- Local Definitions (Should be in a proper header like dirent.h or types.h) ---
 #ifndef DT_DIR // Guard against potential future definition
 #define DT_UNKNOWN 0
 #define DT_FIFO    1
 #define DT_CHR     2
 #define DT_DIR     4 // Directory
 #define DT_BLK     6
 #define DT_REG     8 // Regular file
 #define DT_LNK     10
 #define DT_SOCK    12
 #define DT_WHT     14
 #endif
 // --- End Local Definitions ---
 
 
 // --- Extern Declarations ---
 // Declaration for the global FAT VFS driver structure (defined in fat_core.c)
 extern vfs_driver_t fat_vfs_driver;
 
 // Helper function implementations (moved from previous suggestions/externs)
 // Placeholder formatting function
 static void fat_format_short_name_impl(const uint8_t name_8_3[11], char *out_name) {
     memcpy(out_name, name_8_3, 8);
     int base_len = 8;
     while (base_len > 0 && out_name[base_len - 1] == ' ') base_len--; // Trim trailing spaces from base
     out_name[base_len] = '\0';
     if (name_8_3[8] != ' ') { // If extension exists
         out_name[base_len] = '.';
         base_len++;
         memcpy(out_name + base_len, name_8_3 + 8, 3);
         int ext_len = 3;
         while(ext_len > 0 && out_name[base_len + ext_len - 1] == ' ') ext_len--; // Trim trailing spaces from ext
         out_name[base_len + ext_len] = '\0';
     }
 }
 
 
 /* --- VFS Operation Implementations --- */
 
 /**
  * @brief Opens or creates a file/directory node within the FAT filesystem.
  */
 vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
 {
     fat_fs_t *fs = (fat_fs_t *)fs_context;
     if (!fs || !path) {
         return NULL;
     }
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     fat_dir_entry_t entry;
     char lfn_buffer[FAT_MAX_LFN_CHARS];
     uint32_t entry_dir_cluster = 0;
     uint32_t entry_offset_in_dir = 0;
     int find_res = fat_lookup_path(fs, path,
                                    &entry, lfn_buffer, sizeof(lfn_buffer),
                                    &entry_dir_cluster, &entry_offset_in_dir);
 
     bool exists = (find_res == FS_SUCCESS);
     bool created = false;
     bool truncated = false;
 
     vnode_t *vnode = NULL;
     fat_file_context_t *file_ctx = NULL;
     int ret_err = FS_SUCCESS;
 
     // --- Handle File Creation (O_CREAT) ---
     if (!exists && (flags & O_CREAT)) {
         terminal_printf("[FAT open O_CREAT] '%s' not found, attempting creation.\n", path);
         created = true;
 
         char parent_dir_path[FS_MAX_PATH_LENGTH];
         char new_name[MAX_FILENAME_LEN + 1];
         memset(parent_dir_path, 0, sizeof(parent_dir_path));
         memset(new_name, 0, sizeof(new_name));
 
         if (fs_util_split_path(path, parent_dir_path, sizeof(parent_dir_path),
                              new_name, sizeof(new_name)) != 0)
         {
             ret_err = -FS_ERR_NAMETOOLONG;
             goto open_fail_locked;
         }
         if (strlen(new_name) == 0) {
              ret_err = -FS_ERR_INVALID_PARAM;
              goto open_fail_locked;
         }
 
         fat_dir_entry_t parent_entry;
         uint32_t parent_entry_dir_cluster, parent_entry_offset;
         int parent_res = fat_lookup_path(fs, parent_dir_path,
                                          &parent_entry, NULL, 0,
                                          &parent_entry_dir_cluster, &parent_entry_offset);
 
         if (parent_res != FS_SUCCESS) {
             terminal_printf("[FAT open O_CREAT] Parent dir '%s' not found (err %d).\n", parent_dir_path, parent_res);
             ret_err = parent_res;
             goto open_fail_locked;
         }
         if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
             terminal_printf("[FAT open O_CREAT] Parent path '%s' is not a directory.\n", parent_dir_path);
             ret_err = -FS_ERR_NOT_A_DIRECTORY;
             goto open_fail_locked;
         }
 
         uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
         if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_dir_path, "/") == 0) {
             parent_cluster = 0;
         }
 
         uint8_t short_name[11];
         if (fat_generate_unique_short_name(fs, parent_cluster, new_name, short_name) != 0) {
             terminal_printf("[FAT open O_CREAT] Failed to generate unique short name for '%s'.\n", new_name);
             ret_err = -FS_ERR_NAMETOOLONG;
             goto open_fail_locked;
         }
 
         fat_lfn_entry_t lfn_entries[FAT_MAX_LFN_ENTRIES];
         uint8_t checksum = fat_calculate_lfn_checksum(short_name);
         int lfn_count = fat_generate_lfn_entries(new_name, checksum, lfn_entries, FAT_MAX_LFN_ENTRIES);
         if (lfn_count < 0) {
             terminal_printf("[FAT open O_CREAT] Failed to generate LFN entries for '%s'.\n", new_name);
             ret_err = -FS_ERR_INTERNAL;
             goto open_fail_locked;
         }
         size_t needed_slots = (size_t)lfn_count + 1;
 
         uint32_t slot_cluster, slot_offset;
         int find_slot_res = find_free_directory_slot(fs, parent_cluster, needed_slots,
                                                    &slot_cluster, &slot_offset);
         if (find_slot_res != FS_SUCCESS) {
              terminal_printf("[FAT open O_CREAT] No free directory slots (%u needed) in parent cluster %u (err %d).\n",
                              (unsigned int)needed_slots, parent_cluster, find_slot_res);
              ret_err = find_slot_res;
              goto open_fail_locked;
         }
 
         memset(&entry, 0, sizeof(entry));
         memcpy(entry.name, short_name, 11);
         entry.attr = FAT_ATTR_ARCHIVE;
         entry.file_size = 0;
         entry.first_cluster_low  = 0;
         entry.first_cluster_high = 0;
 
         uint32_t current_write_offset = slot_offset;
         if (lfn_count > 0) {
             if (write_directory_entries(fs, slot_cluster, current_write_offset,
                                         lfn_entries, lfn_count) != FS_SUCCESS)
             {
                 terminal_printf("[FAT open O_CREAT] Failed to write LFN entries.\n");
                 ret_err = -FS_ERR_IO;
                 goto open_fail_locked;
             }
             current_write_offset += (uint32_t)(lfn_count * sizeof(fat_dir_entry_t));
         }
         if (write_directory_entries(fs, slot_cluster, current_write_offset,
                                     &entry, 1) != FS_SUCCESS)
         {
              terminal_printf("[FAT open O_CREAT] Failed to write 8.3 entry.\n");
              ret_err = -FS_ERR_IO;
              goto open_fail_locked;
         }
 
         entry_dir_cluster   = slot_cluster;
         entry_offset_in_dir = current_write_offset;
         exists = true;
 
         buffer_cache_sync();
         terminal_printf("[FAT open O_CREAT] Successfully created '%s'.\n", path);
 
     } else if (!exists) {
         ret_err = find_res;
         goto open_fail_locked;
     }
 
     // --- File/Directory Exists (either found or just created) ---
 
     if ((flags & (O_WRONLY | O_RDWR)) && (entry.attr & FAT_ATTR_DIRECTORY)) {
         ret_err = -FS_ERR_IS_A_DIRECTORY;
         goto open_fail_locked;
     }
     if ((flags & (O_WRONLY | O_RDWR | O_TRUNC)) && (entry.attr & FAT_ATTR_READ_ONLY)) {
          ret_err = -FS_ERR_PERMISSION_DENIED;
          goto open_fail_locked;
     }
 
     // --- Handle File Truncation (O_TRUNC) ---
     if (exists && !created && !(entry.attr & FAT_ATTR_DIRECTORY) && (flags & O_TRUNC)) {
         terminal_printf("[FAT open O_TRUNC] Truncating existing file '%s'.\n", path);
         truncated = true;
 
         uint32_t first_cluster = fat_get_entry_cluster(&entry);
 
         if (first_cluster >= 2) {
             if (fat_free_cluster_chain(fs, first_cluster) != FS_SUCCESS) {
                 terminal_printf("[FAT open O_TRUNC] Error freeing cluster chain for file '%s'.\n", path);
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
             terminal_printf("[FAT open O_TRUNC] Error updating directory entry after truncation.\n");
             ret_err = -FS_ERR_IO;
             goto open_fail_locked;
         }
 
         buffer_cache_sync();
     }
 
     // --- Allocation & Setup for Vnode and File Context ---
     vnode = kmalloc(sizeof(vnode_t));
     file_ctx = kmalloc(sizeof(fat_file_context_t));
     if (!vnode || !file_ctx) {
         terminal_printf("[FAT open] Out of memory allocating vnode/context.\n");
         ret_err = -FS_ERR_OUT_OF_MEMORY;
         goto open_fail_locked;
     }
     memset(vnode, 0, sizeof(*vnode));
     memset(file_ctx, 0, sizeof(*file_ctx));
 
     uint32_t first_cluster_final = fat_get_entry_cluster(&entry);
 
     file_ctx->fs                 = fs;
     file_ctx->first_cluster      = first_cluster_final;
     file_ctx->file_size          = entry.file_size;
     file_ctx->dir_entry_cluster  = entry_dir_cluster;
     file_ctx->dir_entry_offset   = entry_offset_in_dir;
     file_ctx->is_directory       = (entry.attr & FAT_ATTR_DIRECTORY) != 0;
     file_ctx->dirty              = (created || truncated);
 
     file_ctx->readdir_current_cluster = file_ctx->first_cluster;
     if (file_ctx->is_directory && fs->type != FAT_TYPE_FAT32 && file_ctx->first_cluster == 0) {
         file_ctx->readdir_current_cluster = 0;
     }
     file_ctx->readdir_current_offset = 0;
     file_ctx->readdir_last_index     = (size_t)-1;
 
     vnode->data     = file_ctx;
     vnode->fs_driver = &fat_vfs_driver;
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return vnode;
 
 open_fail_locked:
     terminal_printf("[FAT open] Failed for path '%s'. Error code: %d\n", path, ret_err);
     if (vnode)    kfree(vnode);
     if (file_ctx) kfree(file_ctx);
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return NULL;
 }
 
 
 /**
  * @brief Reads the next directory entry from an opened directory.
  */
 int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index)
 {
     if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out) {
         return -FS_ERR_INVALID_PARAM;
     }
     fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
     if (!fctx->fs || !fctx->is_directory) {
         return -FS_ERR_NOT_A_DIRECTORY;
     }
     fat_fs_t *fs = fctx->fs;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     // --- State Management for Sequential Reads ---
     if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
         fctx->readdir_current_cluster = fctx->first_cluster;
         if (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0) {
             fctx->readdir_current_cluster = 0;
         }
         fctx->readdir_current_offset = 0;
         fctx->readdir_last_index = (size_t)-1;
     } else if (entry_index != fctx->readdir_last_index + 1) {
         terminal_printf("[FAT readdir] Warning: Non-sequential index requested (%u requested, %u expected).\n",
                         (unsigned int)entry_index, (unsigned int)(fctx->readdir_last_index + 1));
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_INVALID_PARAM;
     }
 
     uint8_t *sector_buffer = kmalloc(fs->bytes_per_sector);
     if (!sector_buffer) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
     int lfn_count = 0;
     size_t current_logical_index = fctx->readdir_last_index + 1;
     int ret = -FS_ERR_NOT_FOUND;
 
     // --- Directory Scanning Loop ---
     while (true) {
         if (fctx->readdir_current_cluster >= fs->eoc_marker && !(fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0)) {
             ret = -FS_ERR_NOT_FOUND;
             break;
         }
 
         uint32_t sec_size = fs->bytes_per_sector;
         uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
         size_t   offset_in_sector       = fctx->readdir_current_offset % sec_size;
         size_t   entries_per_sector     = sec_size / sizeof(fat_dir_entry_t);
         size_t   entry_index_in_sector  = offset_in_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, fctx->readdir_current_cluster,
                                              sector_offset_in_chain, sector_buffer);
         if (read_res != FS_SUCCESS) {
             ret = (read_res == -FS_ERR_IO) ? -FS_ERR_IO : -FS_ERR_NOT_FOUND;
             break;
         }
 
         for (size_t e_i = entry_index_in_sector; e_i < entries_per_sector; e_i++)
         {
             fat_dir_entry_t *dent = (fat_dir_entry_t*)(sector_buffer + e_i * sizeof(fat_dir_entry_t));
             // uint32_t entry_abs_offset = fctx->readdir_current_offset; // Not needed here
 
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
 
             if (dent->name[0] == FAT_DIR_ENTRY_UNUSED) {
                 ret = -FS_ERR_NOT_FOUND;
                 goto readdir_done;
             }
             if (dent->name[0] == FAT_DIR_ENTRY_DELETED || dent->name[0] == FAT_DIR_ENTRY_KANJI) {
                 lfn_count = 0;
                 continue;
             }
             if ((dent->attr & FAT_ATTR_VOLUME_ID) && !(dent->attr & FAT_ATTR_LONG_NAME)) {
                 lfn_count = 0;
                 continue;
             }
 
             if ((dent->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                 fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                 if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                     lfn_collector[lfn_count++] = *lfn_ent;
                 } else {
                     terminal_printf("[FAT readdir] Warning: LFN entry sequence exceeded buffer (%d entries).\n", FAT_MAX_LFN_ENTRIES);
                     lfn_count = 0;
                 }
                 continue;
             }
             else {
                 // --- 8.3 Entry ---
                 if (current_logical_index == entry_index) {
                     // Found the target entry
                     char final_name[FAT_MAX_LFN_CHARS];
                     final_name[0] = '\0';
 
                     if (lfn_count > 0) {
                         uint8_t expected_sum = fat_calculate_lfn_checksum(dent->name);
                         if (lfn_collector[0].checksum == expected_sum) {
                             fat_reconstruct_lfn(lfn_collector, lfn_count, final_name, sizeof(final_name));
                         } else {
                             terminal_printf("[FAT readdir] LFN checksum mismatch for 8.3 name '%.11s'.\n", (char*)dent->name);
                             lfn_count = 0;
                         }
                     }
 
                     if (final_name[0] == '\0') {
                         fat_format_short_name_impl(dent->name, final_name);
                     }
 
                     strncpy(d_entry_out->d_name, final_name, sizeof(d_entry_out->d_name) - 1);
                     d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0';
 
                     d_entry_out->d_ino = fat_get_entry_cluster(dent);
                     d_entry_out->d_type = (dent->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
 
                     fctx->readdir_last_index = entry_index;
                     ret = FS_SUCCESS;
                     goto readdir_done;
                 }
 
                 current_logical_index++;
                 lfn_count = 0;
             }
         } // End loop through entries in sector
 
         // --- Move to the next sector or cluster ---
         if (!(fs->type != FAT_TYPE_FAT32 && fctx->readdir_current_cluster == 0) &&
             (fctx->readdir_current_offset % fs->cluster_size_bytes == 0))
         {
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, fctx->readdir_current_cluster, &next_c);
             if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) {
                 ret = -FS_ERR_NOT_FOUND;
                 break;
             }
             fctx->readdir_current_cluster = next_c;
             fctx->readdir_current_offset  = 0;
         }
 
     } // End while(true) loop for scanning directory
 
 readdir_done:
     kfree(sector_buffer);
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return ret;
 }
 
 /**
  * @brief Deletes a file from the FAT filesystem.
  */
 int fat_unlink_internal(void *fs_context, const char *path)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs || !path) return -FS_ERR_INVALID_PARAM;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     // --- Reworked unlink logic ---
     // 1. Split path into parent directory path and final component name
     char parent_path[FS_MAX_PATH_LENGTH];
     char component_name[MAX_FILENAME_LEN + 1];
     if (fs_util_split_path(path, parent_path, sizeof(parent_path), component_name, sizeof(component_name)) != 0) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_NAMETOOLONG;
     }
      if (strlen(component_name) == 0) { // Cannot unlink empty name component
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_INVALID_PARAM;
     }
 
     // 2. Lookup the parent directory
     fat_dir_entry_t parent_entry;
     uint32_t parent_entry_dir_cluster, parent_entry_offset;
     int parent_res = fat_lookup_path(fs, parent_path, &parent_entry, NULL, 0,
                                      &parent_entry_dir_cluster, &parent_entry_offset);
     if (parent_res != FS_SUCCESS) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return parent_res; // Parent not found
     }
     if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_NOT_A_DIRECTORY; // Parent path is not a directory
     }
     uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
     if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_path, "/") == 0) {
          parent_cluster = 0; // Handle FAT12/16 root
     }
 
     // 3. Find the actual entry within the parent directory using the component name
     fat_dir_entry_t entry_to_delete;
     uint32_t entry_offset;
     uint32_t first_lfn_offset = (uint32_t)-1; // Get LFN start offset
     int find_res = fat_find_in_dir(fs, parent_cluster, component_name,
                                    &entry_to_delete, NULL, 0, // Don't need LFN name buffer here
                                    &entry_offset, &first_lfn_offset);
 
     if (find_res != FS_SUCCESS) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return find_res; // File/component not found in parent directory
     }
 
     // Check if it's a directory - cannot unlink directories with this function
     if (entry_to_delete.attr & FAT_ATTR_DIRECTORY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_IS_A_DIRECTORY; // Use rmdir instead
     }
 
     // Check if it's read-only
     if (entry_to_delete.attr & FAT_ATTR_READ_ONLY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_PERMISSION_DENIED;
     }
 
     // --- Free cluster chain ---
     uint32_t file_cluster = fat_get_entry_cluster(&entry_to_delete);
     if (file_cluster >= 2) {
         int free_res = fat_free_cluster_chain(fs, file_cluster);
         if (free_res != FS_SUCCESS) {
             terminal_printf("[FAT unlink] Warning: Error freeing cluster chain for '%s' (err %d).\n", path, free_res);
             // Continue to mark entry deleted anyway
         }
     }
 
     // --- Mark directory entries as deleted ---
     size_t num_entries_to_mark = 1;
     uint32_t mark_start_offset = entry_offset; // Offset of the 8.3 entry
 
     if (first_lfn_offset != (uint32_t)-1 && first_lfn_offset < entry_offset) {
         num_entries_to_mark = ((entry_offset - first_lfn_offset) / sizeof(fat_dir_entry_t)) + 1;
         mark_start_offset = first_lfn_offset;
     }
 
     int mark_res = mark_directory_entries_deleted(fs, parent_cluster,
                                                 mark_start_offset,
                                                 num_entries_to_mark,
                                                 FAT_DIR_ENTRY_DELETED);
     if (mark_res != FS_SUCCESS) {
         terminal_printf("[FAT unlink] Error marking directory entry deleted for '%s' (err %d).\n", path, mark_res);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return mark_res;
     }
 
     // --- Flush changes ---
     buffer_cache_sync();
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     terminal_printf("[FAT unlink] Successfully unlinked '%s'.\n", path);
     return FS_SUCCESS;
 }
 
 
 /* --- Internal Helper Implementations --- */
 
 /**
  * @brief Looks up a single path component within a directory cluster.
  */
 int fat_find_in_dir(fat_fs_t *fs,
                     uint32_t dir_cluster,
                     const char *component,
                     fat_dir_entry_t *entry_out,
                     char *lfn_out, size_t lfn_max_len,
                     uint32_t *entry_offset_in_dir_out,
                     uint32_t *first_lfn_offset_out)
 {
     KERNEL_ASSERT(fs != NULL && component != NULL && entry_out != NULL && entry_offset_in_dir_out != NULL,
                   "NULL pointer passed to fat_find_in_dir for required arguments");
 
     uint32_t current_cluster = dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
     uint32_t current_byte_offset = 0;
 
     if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
     if (first_lfn_offset_out) *first_lfn_offset_out = (uint32_t)-1;
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) return -FS_ERR_OUT_OF_MEMORY;
 
     fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
     int lfn_count = 0;
     uint32_t current_lfn_start_offset = (uint32_t)-1;
 
     while (true) {
          if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
              break;
          }
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             kfree(sector_data);
             return -FS_ERR_NOT_FOUND;
         }
 
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                 kfree(sector_data);
                 return -FS_ERR_NOT_FOUND;
             }
             if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) {
                 lfn_count = 0;
                 current_lfn_start_offset = (uint32_t)-1;
                 continue;
             }
              if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) {
                 lfn_count = 0;
                 current_lfn_start_offset = (uint32_t)-1;
                 continue;
             }
 
             if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                 if (lfn_count == 0) {
                     current_lfn_start_offset = entry_abs_offset;
                 }
                 if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                     lfn_collector[lfn_count++] = *((fat_lfn_entry_t*)de);
                 } else {
                     lfn_count = 0;
                     current_lfn_start_offset = (uint32_t)-1;
                 }
             } else {
                 // 8.3 Entry - check for match
                 bool match = false;
                 char reconstructed_lfn_buf[FAT_MAX_LFN_CHARS];
 
                 if (lfn_count > 0) {
                     uint8_t expected_sum = fat_calculate_lfn_checksum(de->name);
                     if (lfn_collector[0].checksum == expected_sum) {
                         fat_reconstruct_lfn(lfn_collector, lfn_count, reconstructed_lfn_buf, sizeof(reconstructed_lfn_buf));
                         // Ensure prototype exists in fat_utils.h!
                         if (fat_compare_lfn(component, reconstructed_lfn_buf) == 0) {
                             match = true;
                             if (lfn_out && lfn_max_len > 0) {
                                 strncpy(lfn_out, reconstructed_lfn_buf, lfn_max_len - 1);
                                 lfn_out[lfn_max_len - 1] = '\0';
                             }
                         }
                     } else {
                         lfn_count = 0;
                         current_lfn_start_offset = (uint32_t)-1;
                     }
                 }
 
                 if (!match) {
                      // Ensure prototype exists in fat_utils.h!
                     if (fat_compare_8_3(component, de->name) == 0) {
                         match = true;
                         if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
                         current_lfn_start_offset = (uint32_t)-1;
                     }
                 }
 
                 if (match) {
                     memcpy(entry_out, de, sizeof(fat_dir_entry_t));
                     *entry_offset_in_dir_out = entry_abs_offset;
                     if (first_lfn_offset_out) {
                         *first_lfn_offset_out = current_lfn_start_offset;
                     }
                     kfree(sector_data);
                     return FS_SUCCESS;
                 }
 
                 lfn_count = 0;
                 current_lfn_start_offset = (uint32_t)-1;
             }
         } // End loop through entries in sector
 
         current_byte_offset += fs->bytes_per_sector;
 
         if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
             if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) {
                  break;
             }
             current_cluster = next_c;
             current_byte_offset = 0;
         }
     }
 
     kfree(sector_data);
     return -FS_ERR_NOT_FOUND;
 }
 
 
 /**
  * @brief Resolves a full absolute path to its final directory entry.
  */
 int fat_lookup_path(fat_fs_t *fs, const char *path,
                     fat_dir_entry_t *entry_out,
                     char *lfn_out, size_t lfn_max_len,
                     uint32_t *entry_dir_cluster_out,
                     uint32_t *entry_offset_in_dir_out)
 {
     KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && entry_dir_cluster_out != NULL && entry_offset_in_dir_out != NULL,
                   "NULL pointer passed to fat_lookup_path for required arguments");
 
     if (path[0] != '/') {
         return -FS_ERR_INVALID_PARAM;
     }
 
     if (strcmp(path, "/") == 0) {
         memset(entry_out, 0, sizeof(*entry_out));
         entry_out->attr = FAT_ATTR_DIRECTORY;
         *entry_offset_in_dir_out = 0;
 
         if (fs->type == FAT_TYPE_FAT32) {
             entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
             entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
             *entry_dir_cluster_out = 0;
         } else {
             entry_out->first_cluster_low  = 0;
             entry_out->first_cluster_high = 0;
             *entry_dir_cluster_out = 0;
         }
 
         if (lfn_out && lfn_max_len > 0) {
             strncpy(lfn_out, "/", lfn_max_len -1);
             lfn_out[lfn_max_len - 1] = '\0';
         }
         return FS_SUCCESS;
     }
 
     // --- Tokenize the path ---
     char *path_copy = kmalloc(strlen(path) + 1);
     if (!path_copy) return -FS_ERR_OUT_OF_MEMORY;
     strcpy(path_copy, path);
 
     // IMPORTANT: Use strtok correctly (only 2 arguments)
     char *component = strtok(path_copy + 1, "/");
 
     // --- Path Traversal ---
     uint32_t current_dir_cluster;
     fat_dir_entry_t current_entry;
 
     if (fs->type == FAT_TYPE_FAT32) {
         current_dir_cluster = fs->root_cluster;
     } else {
         current_dir_cluster = 0;
     }
     memset(&current_entry, 0, sizeof(current_entry));
     current_entry.attr = FAT_ATTR_DIRECTORY;
 
     uint32_t previous_dir_cluster = 0;
 
     while (component != NULL) {
         if (strlen(component) == 0) {
             component = strtok(NULL, "/"); // Correct strtok call
             continue;
         }
 
         if (strcmp(component, ".") == 0) {
             component = strtok(NULL, "/"); // Correct strtok call
             continue;
         }
         if (strcmp(component, "..") == 0) {
             terminal_printf("[FAT lookup] Error: '..' component not yet supported.\n");
             kfree(path_copy);
             return -FS_ERR_NOT_SUPPORTED;
         }
 
         previous_dir_cluster = current_dir_cluster;
 
         uint32_t component_entry_offset;
         int find_comp_res = fat_find_in_dir(fs, current_dir_cluster, component,
                                             &current_entry,
                                             lfn_out, lfn_max_len,
                                             &component_entry_offset, NULL);
 
         if (find_comp_res != FS_SUCCESS) {
             kfree(path_copy);
             return -FS_ERR_NOT_FOUND;
         }
 
         // Check if we are done (last component found)
         char* next_component = strtok(NULL, "/"); // Correct strtok call
         if (next_component == NULL) {
             memcpy(entry_out, &current_entry, sizeof(*entry_out));
             *entry_dir_cluster_out = previous_dir_cluster;
             *entry_offset_in_dir_out = component_entry_offset;
             kfree(path_copy);
             return FS_SUCCESS;
         }
 
         if (!(current_entry.attr & FAT_ATTR_DIRECTORY)) {
             kfree(path_copy);
             return -FS_ERR_NOT_A_DIRECTORY;
         }
 
         current_dir_cluster = fat_get_entry_cluster(&current_entry);
         if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0) {
             terminal_printf("[FAT lookup] Warning: Traversed into FAT12/16 root unexpectedly.\n");
         }
         component = next_component;
 
     } // End while(component != NULL)
 
     kfree(path_copy);
     // Should have returned success inside the loop if path was valid
     return -FS_ERR_NOT_FOUND; // Or internal error if loop exit condition was unexpected
 }
 
 /**
  * @brief Reads a specific sector from a directory cluster chain.
  */
 int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                           uint32_t sector_offset_in_chain,
                           uint8_t* buffer)
 {
     KERNEL_ASSERT(fs != NULL && buffer != NULL, "FS context and buffer cannot be NULL in read_directory_sector");
 
     if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         // FAT12/16 root directory
         if (sector_offset_in_chain >= fs->root_dir_sectors) {
             return -FS_ERR_NOT_FOUND;
         }
         uint32_t lba = fs->root_dir_start_lba + sector_offset_in_chain;
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else if (cluster >= 2) {
         // Regular directory
         uint32_t current_cluster = cluster;
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
 
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
             uint32_t next_cluster;
             if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != FS_SUCCESS) {
                 return -FS_ERR_IO;
             }
             if (next_cluster >= fs->eoc_marker) {
                 return -FS_ERR_NOT_FOUND;
             }
             current_cluster = next_cluster;
         }
 
         uint32_t lba = fat_cluster_to_lba(fs, current_cluster);
         if (lba == 0) return -FS_ERR_IO;
         lba += sector_in_final_cluster;
 
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else {
         return -FS_ERR_INVALID_PARAM;
     }
 }
 
 /**
  * @brief Updates an existing 8.3 directory entry on disk.
  */
 int update_directory_entry(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const fat_dir_entry_t *new_entry)
 {
     KERNEL_ASSERT(fs != NULL && new_entry != NULL, "FS context and new entry cannot be NULL in update_directory_entry");
 
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
     KERNEL_ASSERT(offset_in_sector + sizeof(fat_dir_entry_t) <= sector_size,
                   "Directory entry write crosses sector boundary");
 
     // --- Determine LBA ---
     uint32_t lba;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
          if (sector_offset_in_chain >= fs->root_dir_sectors) return -FS_ERR_INVALID_PARAM;
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) {
         uint32_t current_cluster = dir_cluster;
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
 
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
              uint32_t next_cluster;
              if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != FS_SUCCESS) return -FS_ERR_IO;
              if (next_cluster >= fs->eoc_marker) return -FS_ERR_INVALID_PARAM;
              current_cluster = next_cluster;
         }
         lba = fat_cluster_to_lba(fs, current_cluster);
         if (lba == 0) return -FS_ERR_IO;
         lba += sector_in_final_cluster;
     } else {
         return -FS_ERR_INVALID_PARAM;
     }
 
     buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
     if (!b) return -FS_ERR_IO;
 
     memcpy(b->data + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
 
     buffer_mark_dirty(b);
     buffer_release(b);
 
     return FS_SUCCESS;
 }
 
 
 /**
  * @brief Marks one or more directory entries as deleted.
  */
 int mark_directory_entries_deleted(fat_fs_t *fs,
                                    uint32_t dir_cluster,
                                    uint32_t first_entry_offset,
                                    size_t num_entries,
                                    uint8_t marker)
 {
     KERNEL_ASSERT(fs != NULL && num_entries > 0, "FS context must be valid and num_entries > 0");
 
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     int result = FS_SUCCESS;
     size_t entries_marked = 0;
     uint32_t current_offset = first_entry_offset;
 
     while (entries_marked < num_entries) {
         uint32_t sector_offset_in_chain = current_offset / sector_size;
         size_t offset_in_sector = current_offset % sector_size;
 
         uint32_t lba;
          if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
              if (sector_offset_in_chain >= fs->root_dir_sectors) { result = -FS_ERR_INVALID_PARAM; break; }
              lba = fs->root_dir_start_lba + sector_offset_in_chain;
          } else if (dir_cluster >= 2) {
              uint32_t current_data_cluster = dir_cluster;
              uint32_t sectors_per_cluster = fs->sectors_per_cluster;
              uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
              uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
              for (uint32_t i = 0; i < cluster_hop_count; i++) {
                  uint32_t next_cluster;
                  if (fat_get_next_cluster(fs, current_data_cluster, &next_cluster) != FS_SUCCESS) { result = -FS_ERR_IO; goto mark_fail; }
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto mark_fail; }
                  current_data_cluster = next_cluster;
              }
              lba = fat_cluster_to_lba(fs, current_data_cluster);
              if (lba == 0) { result = -FS_ERR_IO; break; }
              lba += sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break;
          }
 
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; }
 
         bool buffer_dirtied = false;
         while (entries_marked < num_entries && offset_in_sector < sector_size) {
             fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(b->data + offset_in_sector);
             entry_ptr->name[0] = marker;
             buffer_dirtied = true;
             offset_in_sector += sizeof(fat_dir_entry_t);
             current_offset   += sizeof(fat_dir_entry_t);
             entries_marked++;
         }
 
         if (buffer_dirtied) {
             buffer_mark_dirty(b);
         }
         buffer_release(b);
 
         if (result != FS_SUCCESS) break;
 
     } // end while
 
 mark_fail:
     return result;
 }
 
 
 /**
  * @brief Writes one or more consecutive directory entries to disk.
  */
 int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries)
 {
     KERNEL_ASSERT(fs != NULL && entries_buf != NULL, "FS context and entry buffer cannot be NULL in write_directory_entries");
     if (num_entries == 0) return FS_SUCCESS;
 
     size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     const uint8_t *src_buf = (const uint8_t *)entries_buf;
     size_t bytes_written = 0;
     int result = FS_SUCCESS;
 
     while (bytes_written < total_bytes) {
         uint32_t current_abs_offset = dir_offset + (uint32_t)bytes_written;
         uint32_t sector_offset_in_chain = current_abs_offset / sector_size;
         size_t offset_in_sector = current_abs_offset % sector_size;
 
         uint32_t lba;
          if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
              if (sector_offset_in_chain >= fs->root_dir_sectors) { result = -FS_ERR_INVALID_PARAM; break; }
              lba = fs->root_dir_start_lba + sector_offset_in_chain;
          } else if (dir_cluster >= 2) {
              uint32_t current_data_cluster = dir_cluster;
              uint32_t sectors_per_cluster = fs->sectors_per_cluster;
              uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
              uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
              for (uint32_t i = 0; i < cluster_hop_count; i++) {
                  uint32_t next_cluster;
                  if (fat_get_next_cluster(fs, current_data_cluster, &next_cluster) != FS_SUCCESS) { result = -FS_ERR_IO; goto write_fail; }
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto write_fail; }
                  current_data_cluster = next_cluster;
              }
              lba = fat_cluster_to_lba(fs, current_data_cluster);
               if (lba == 0) { result = -FS_ERR_IO; break; }
              lba += sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break;
          }
 
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; }
 
         size_t bytes_to_write_this_sector = sector_size - offset_in_sector;
         size_t bytes_remaining_total = total_bytes - bytes_written;
         if (bytes_to_write_this_sector > bytes_remaining_total) {
             bytes_to_write_this_sector = bytes_remaining_total;
         }
 
         memcpy(b->data + offset_in_sector, src_buf + bytes_written, bytes_to_write_this_sector);
 
         buffer_mark_dirty(b);
         buffer_release(b);
 
         bytes_written += bytes_to_write_this_sector;
 
     } // end while
 
 write_fail:
     return result;
 }
 
 
 /**
  * @brief Finds a sequence of free slots in a directory.
  * @warning Basic placeholder implementation. Needs directory extension logic.
  */
 int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                              size_t needed_slots,
                              uint32_t *out_slot_cluster,
                              uint32_t *out_slot_offset)
 {
     KERNEL_ASSERT(fs != NULL && needed_slots > 0 && out_slot_cluster != NULL && out_slot_offset != NULL,
                   "Invalid arguments passed to find_free_directory_slot");
 
     terminal_printf("[FAT find_free_directory_slot] Warning: Using basic placeholder implementation.\n");
 
     uint32_t current_cluster = parent_dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && parent_dir_cluster == 0);
     uint32_t current_byte_offset = 0;
     size_t contiguous_free_count = 0;
     uint32_t potential_start_offset = 0;
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) return -FS_ERR_OUT_OF_MEMORY;
 
     while (true) {
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break;
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             break;
         }
 
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED || de->name[0] == FAT_DIR_ENTRY_DELETED) {
                 if (contiguous_free_count == 0) {
                     potential_start_offset = entry_abs_offset;
                 }
                 contiguous_free_count++;
                 if (contiguous_free_count >= needed_slots) {
                     *out_slot_cluster = current_cluster;
                     *out_slot_offset = potential_start_offset;
                     kfree(sector_data);
                     return FS_SUCCESS;
                 }
             } else {
                 contiguous_free_count = 0;
             }
 
              if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                  goto find_slot_fail;
             }
         }
 
          current_byte_offset += fs->bytes_per_sector;
          if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
              uint32_t next_c;
              int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
              if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) break;
              current_cluster = next_c;
              current_byte_offset = 0;
          }
 
     } // End while
 
 find_slot_fail:
     kfree(sector_data);
     terminal_printf("[FAT find_free_directory_slot] No contiguous slot found (%u needed).\n", (unsigned int)needed_slots);
     if (scanning_fixed_root) {
          terminal_printf("[FAT find_free_directory_slot] Cannot extend FAT12/16 root directory.\n");
     } else {
          terminal_printf("[FAT find_free_directory_slot] Directory extension logic not implemented.\n");
          // TODO: Implement directory extension here
          // 1. Allocate a new cluster using fat_allocate_cluster
          // 2. Link the new cluster to the current_cluster using fat_set_cluster_entry
          // 3. Zero out the new cluster using buffer cache / disk_write_raw_sectors
          // 4. Set out_slot_cluster to the new cluster, out_slot_offset to 0
          // 5. Return FS_SUCCESS
     }
     return -FS_ERR_NO_SPACE;
 }
 
 
 // Added implementation for fat_raw_short_name_exists
 bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]) {
     KERNEL_ASSERT(fs != NULL && short_name_raw != NULL, "NULL fs or name pointer");
     // Assertion: Assumes caller holds fs->lock
 
     uint32_t current_cluster = dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
     uint32_t current_byte_offset = 0;
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) {
         terminal_printf("[RawExists] Failed to allocate sector buffer.\n");
         return false; // Indicate error / cannot check
     }
 
     while (true) {
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break;
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) break; // End of dir or IO error
 
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
 
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) goto not_found; // End marker
             if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) continue;
             if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) continue;
 
             // Compare raw 11 bytes
             if (memcmp(de->name, short_name_raw, 11) == 0) {
                 kfree(sector_data);
                 return true; // Found match
             }
         }
 
         current_byte_offset += fs->bytes_per_sector;
         if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             uint32_t next_c;
             if (fat_get_next_cluster(fs, current_cluster, &next_c) != FS_SUCCESS || next_c >= fs->eoc_marker) break;
             current_cluster = next_c;
             current_byte_offset = 0;
         }
     }
 
 not_found:
     kfree(sector_data);
     return false; // Not found
 }