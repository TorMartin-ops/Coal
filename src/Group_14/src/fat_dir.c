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
 #include "fat_utils.h"  // FAT entry access, LBA conversion, name formatting etc.
 #include "fat_lfn.h"    // LFN specific helpers (checksum, reconstruct, generate)
 #include "fat_io.h"     // read_cluster_cached, write_cluster_cached (though less used directly here)
 #include "buffer_cache.h" // Buffer cache access (buffer_get, buffer_release, etc.)
 #include "spinlock.h"   // Locking primitives
 #include "terminal.h"   // Logging
 #include "sys_file.h"   // O_* flags definitions
 #include "kmalloc.h"    // Kernel memory allocation
 #include "fs_errno.h"   // Filesystem error codes
 #include "fs_limits.h"  // Filesystem limits (e.g., MAX_PATH_LENGTH)
 #include <string.h>     // memcpy, memcmp, memset, strlen, strchr, strrchr, strtok_r (safer alternative maybe?)
 #include "assert.h"     // KERNEL_ASSERT
 
 /* --- Static Helper Prototypes --- */
 
 // (Add prototypes for any static helpers defined within this file, if needed)
 
 
 /* --- VFS Operation Implementations --- */
 
 /**
  * @brief Opens or creates a file/directory node within the FAT filesystem.
  */
 vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
 {
     fat_fs_t *fs = (fat_fs_t *)fs_context;
     if (!fs || !path) {
         // fs_set_errno(-FS_ERR_INVALID_PARAM); // Consider setting thread-local errno
         return NULL;
     }
 
     // Need lock for consistent lookup and potential creation/truncation
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     fat_dir_entry_t entry;
     char lfn_buffer[FAT_MAX_LFN_CHARS]; // Buffer for LFN lookup result
     uint32_t entry_dir_cluster = 0;     // Cluster of the PARENT directory
     uint32_t entry_offset_in_dir = 0;   // Offset of the entry within the parent
     int find_res = fat_lookup_path(fs, path,
                                     &entry, lfn_buffer, sizeof(lfn_buffer),
                                     &entry_dir_cluster, &entry_offset_in_dir);
 
     bool exists = (find_res == FS_SUCCESS);
     bool created = false;
     bool truncated = false;
 
     vnode_t *vnode = NULL;
     fat_file_context_t *file_ctx = NULL;
     int ret_err = FS_SUCCESS; // Track potential errors
 
     // --- Handle File Creation (O_CREAT) ---
     if (!exists && (flags & O_CREAT)) {
         terminal_printf("[FAT open O_CREAT] '%s' not found, attempting creation.\n", path);
         created = true;
 
         // 1. Separate parent path and new filename component
         char parent_dir_path[FS_MAX_PATH_LENGTH]; // Use FS_MAX_PATH_LENGTH or similar
         char new_name[MAX_FILENAME_LEN + 1]; // Max LFN component length
         memset(parent_dir_path, 0, sizeof(parent_dir_path));
         memset(new_name, 0, sizeof(new_name));
 
         // Use a robust path splitting function (assuming fat_utils.c has one)
         if (fat_split_path(path, parent_dir_path, sizeof(parent_dir_path),
                              new_name, sizeof(new_name)) != 0)
         {
             ret_err = -FS_ERR_NAMETOOLONG; // Or appropriate error
             goto open_fail_locked;
         }
         if (strlen(new_name) == 0) { // Cannot create empty name component
              ret_err = -FS_ERR_INVALID_PARAM;
              goto open_fail_locked;
         }
 
         // 2. Lookup the parent directory to get its cluster number
         fat_dir_entry_t parent_entry;
         uint32_t parent_entry_dir_cluster, parent_entry_offset; // We don't really need offset here
         int parent_res = fat_lookup_path(fs, parent_dir_path,
                                          &parent_entry, NULL, 0, // Don't need parent LFN
                                          &parent_entry_dir_cluster, &parent_entry_offset);
 
         if (parent_res != FS_SUCCESS) {
             terminal_printf("[FAT open O_CREAT] Parent dir '%s' not found (err %d).\n", parent_dir_path, parent_res);
             ret_err = parent_res; // Propagate error (e.g., -FS_ERR_NOT_FOUND)
             goto open_fail_locked;
         }
         if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
             terminal_printf("[FAT open O_CREAT] Parent path '%s' is not a directory.\n", parent_dir_path);
             ret_err = -FS_ERR_NOT_A_DIRECTORY;
             goto open_fail_locked;
         }
 
         // Get the actual cluster number of the parent directory
         uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
         // Handle FAT12/16 root directory special case (cluster 0)
         if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_dir_path, "/") == 0) {
             parent_cluster = 0;
         }
 
         // 3. Generate a unique 8.3 short name
         uint8_t short_name[11];
         // Assuming fat_lfn.c provides generate_unique_short_name
         if (fat_generate_unique_short_name(fs, parent_cluster, new_name, short_name) != 0) {
             terminal_printf("[FAT open O_CREAT] Failed to generate unique short name for '%s'.\n", new_name);
             ret_err = -FS_ERR_NAMETOOLONG; // Or maybe -FS_ERR_EXISTS if collision logic fails
             goto open_fail_locked;
         }
 
         // 4. Generate LFN entries if needed
         fat_lfn_entry_t lfn_entries[FAT_MAX_LFN_ENTRIES];
         // Assuming fat_lfn.c provides generate_lfn_entries
         int lfn_count = fat_generate_lfn_entries(new_name, short_name, lfn_entries, FAT_MAX_LFN_ENTRIES);
         if (lfn_count < 0) { // Check for error from generate_lfn_entries
             terminal_printf("[FAT open O_CREAT] Failed to generate LFN entries for '%s'.\n", new_name);
             ret_err = -FS_ERR_INTERNAL; // Or specific error
             goto open_fail_locked;
         }
         size_t needed_slots = (size_t)lfn_count + 1; // LFNs + 1 for 8.3 entry
 
         // 5. Find free slot(s) in the parent directory
         uint32_t slot_cluster, slot_offset;
         int find_slot_res = find_free_directory_slot(fs, parent_cluster, needed_slots,
                                                   &slot_cluster, &slot_offset);
         if (find_slot_res != FS_SUCCESS) {
              terminal_printf("[FAT open O_CREAT] No free directory slots (%u needed) in parent cluster %u (err %d).\n",
                              needed_slots, parent_cluster, find_slot_res);
              ret_err = find_slot_res; // -FS_ERR_NO_SPACE expected
              goto open_fail_locked;
         }
 
         // 6. Prepare the new 8.3 directory entry
         memset(&entry, 0, sizeof(entry)); // Zero out entry, especially time/date/cluster/size
         memcpy(entry.name, short_name, 11);
         entry.attr = FAT_ATTR_ARCHIVE; // Standard attribute for new files
         // TODO: Set creation time/date here using system time
         entry.file_size = 0;
         entry.first_cluster_low  = 0; // No cluster allocated yet
         entry.first_cluster_high = 0;
 
         // 7. Write LFN entries (if any) then the 8.3 entry
         uint32_t current_write_offset = slot_offset;
         if (lfn_count > 0) {
             if (write_directory_entries(fs, slot_cluster, current_write_offset,
                                         lfn_entries, lfn_count) != FS_SUCCESS)
             {
                 terminal_printf("[FAT open O_CREAT] Failed to write LFN entries.\n");
                 ret_err = -FS_ERR_IO;
                 // Attempt to mark slots as deleted again? Complex recovery.
                 goto open_fail_locked;
             }
             current_write_offset += (uint32_t)(lfn_count * sizeof(fat_dir_entry_t));
         }
         // Write the final 8.3 entry
         if (write_directory_entries(fs, slot_cluster, current_write_offset,
                                     &entry, 1) != FS_SUCCESS)
         {
              terminal_printf("[FAT open O_CREAT] Failed to write 8.3 entry.\n");
              ret_err = -FS_ERR_IO;
              // Attempt recovery?
              goto open_fail_locked;
         }
 
         // Creation successful, update state for proceeding
         entry_dir_cluster   = slot_cluster;
         entry_offset_in_dir = current_write_offset; // Offset of the 8.3 entry
         exists = true; // The file now exists
 
         // Ensure directory changes are flushed (important!)
         // Flush FAT cache as well if directory extension allocated clusters (handled in find_free_directory_slot ideally)
         // flush_fat_table(fs); // Maybe only if find_free_directory_slot allocated
         buffer_cache_sync_device(fs->disk_ptr->blk_dev.device_name); // Sync device containing the dir
 
         terminal_printf("[FAT open O_CREAT] Successfully created '%s'.\n", path);
 
     } else if (!exists) {
         // File doesn't exist, and O_CREAT was not specified
         ret_err = find_res; // Should be -FS_ERR_NOT_FOUND from lookup
         goto open_fail_locked;
     }
 
     // --- File/Directory Exists (either found or just created) ---
 
     // Check permissions based on existing/new entry attributes and open flags
     // Cannot open directory for writing
     if ((flags & (O_WRONLY | O_RDWR)) && (entry.attr & FAT_ATTR_DIRECTORY)) {
         ret_err = -FS_ERR_IS_A_DIRECTORY;
         goto open_fail_locked;
     }
     // Cannot open read-only file for writing (unless O_TRUNC is also used, maybe?)
     // Standard POSIX allows truncating RO files if user has write permission on directory.
     // FAT doesn't have directory permissions, so let's just deny based on RO attribute for simplicity.
     if ((flags & (O_WRONLY | O_RDWR | O_TRUNC)) && (entry.attr & FAT_ATTR_READ_ONLY)) {
          ret_err = -FS_ERR_PERMISSION_DENIED;
          goto open_fail_locked;
     }
 
     // --- Handle File Truncation (O_TRUNC) ---
     if (exists && !created && !(entry.attr & FAT_ATTR_DIRECTORY) && (flags & O_TRUNC)) {
         terminal_printf("[FAT open O_TRUNC] Truncating existing file '%s'.\n", path);
         truncated = true;
 
         // Get the first cluster of the file to free the chain
         uint32_t first_cluster = fat_get_entry_cluster(&entry);
 
         if (first_cluster >= 2) {
             // Free the cluster chain associated with the file
             // Assuming fat_alloc.c provides fat_free_cluster_chain
             if (fat_free_cluster_chain(fs, first_cluster) != FS_SUCCESS) {
                 terminal_printf("[FAT open O_TRUNC] Error freeing cluster chain for file '%s'.\n", path);
                 ret_err = -FS_ERR_IO; // Or more specific error from free_chain
                 goto open_fail_locked;
             }
         }
 
         // Update the directory entry: zero size and first cluster
         entry.file_size = 0;
         entry.first_cluster_low  = 0;
         entry.first_cluster_high = 0;
         // TODO: Update modification time/date here
         if (update_directory_entry(fs, entry_dir_cluster, entry_offset_in_dir,
                                      &entry) != FS_SUCCESS)
         {
             terminal_printf("[FAT open O_TRUNC] Error updating directory entry after truncation.\n");
             ret_err = -FS_ERR_IO;
             goto open_fail_locked;
         }
 
         // Ensure changes (FAT and directory entry) are persistent
         // flush_fat_table(fs); // free_chain should mark FAT dirty
         buffer_cache_sync_device(fs->disk_ptr->blk_dev.device_name); // Sync device with FAT/dir data
     }
 
     // --- Allocation & Setup for Vnode and File Context ---
     vnode = kmalloc(sizeof(vnode_t));
     file_ctx = kmalloc(sizeof(fat_file_context_t));
     if (!vnode || !file_ctx) {
         terminal_printf("[FAT open] Out of memory allocating vnode/context.\n");
         ret_err = -FS_ERR_OUT_OF_MEMORY;
         goto open_fail_locked; // Free partially allocated memory
     }
     memset(vnode, 0, sizeof(*vnode));
     memset(file_ctx, 0, sizeof(*file_ctx));
 
     // Fill the FAT-specific file context
     uint32_t first_cluster_final = fat_get_entry_cluster(&entry); // Use potentially updated entry
 
     file_ctx->fs                  = fs;
     file_ctx->first_cluster       = first_cluster_final;
     file_ctx->file_size           = entry.file_size;
     file_ctx->dir_entry_cluster   = entry_dir_cluster;   // Parent dir cluster
     file_ctx->dir_entry_offset    = entry_offset_in_dir; // Offset of 8.3 entry
     file_ctx->is_directory        = (entry.attr & FAT_ATTR_DIRECTORY) != 0;
     file_ctx->dirty               = (created || truncated); // Mark dirty if created/truncated
 
     // Initialize readdir state (relevant only for directories)
     file_ctx->readdir_current_cluster = file_ctx->first_cluster;
     // Handle FAT12/16 root case for readdir start
     if (file_ctx->is_directory && fs->type != FAT_TYPE_FAT32 && file_ctx->first_cluster == 0) {
         file_ctx->readdir_current_cluster = 0; // Use 0 marker for root scan
     }
     file_ctx->readdir_current_offset  = 0;
     file_ctx->readdir_last_index      = (size_t)-1; // Indicate readdir hasn't started
 
     // Link context to vnode
     vnode->data      = file_ctx;
     vnode->fs_driver = &fat_vfs_driver; // Reference the driver struct defined in fat_core.c
 
     // Release lock and return success
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return vnode;
 
 open_fail_locked:
     // Cleanup logic if open fails after acquiring lock
     terminal_printf("[FAT open] Failed for path '%s'. Error code: %d\n", path, ret_err);
     if (vnode)    kfree(vnode);
     if (file_ctx) kfree(file_ctx);
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     // fs_set_errno(ret_err); // Set thread-local errno maybe
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
         // Must be an opened directory
         return -FS_ERR_NOT_A_DIRECTORY; // Or FS_ERR_INVALID_PARAM
     }
     fat_fs_t *fs = fctx->fs;
 
     // Readdir needs consistent view of directory entries
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     // --- State Management for Sequential Reads ---
     // If user seeks (by requesting index 0) or requests an earlier index, reset scan state.
     // Otherwise, expect entry_index to be one greater than the last returned index.
     if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
         // Reset scan state to the beginning of the directory
         fctx->readdir_current_cluster = fctx->first_cluster;
         if (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0) {
             fctx->readdir_current_cluster = 0; // Special marker for FAT12/16 root
         }
         fctx->readdir_current_offset = 0;
         fctx->readdir_last_index = (size_t)-1; // Reset index tracking
     } else if (entry_index != fctx->readdir_last_index + 1) {
         // User skipped an index - POSIX allows this, but it's inefficient for us.
         // We could potentially seek, but for now, let's return an error or require sequential access.
         // Or, we could just reset and scan from start (inefficient).
         // Let's treat non-sequential reads as an error for simplicity here.
         terminal_printf("[FAT readdir] Warning: Non-sequential index requested (%u requested, %u expected).\n",
                         entry_index, fctx->readdir_last_index + 1);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_INVALID_PARAM; // Or maybe scan from start if we want to support it
     }
 
     // Allocate buffer for reading sectors
     uint8_t *sector_buffer = kmalloc(fs->bytes_per_sector);
     if (!sector_buffer) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_OUT_OF_MEMORY;
     }
 
     fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES]; // Buffer to collect LFN entries
     int lfn_count = 0;                                 // Number of LFN entries collected for the current 8.3 entry
     size_t current_logical_index = fctx->readdir_last_index + 1; // The index we are searching for
     int ret = -FS_ERR_NOT_FOUND; // Default to not found
 
     // --- Directory Scanning Loop ---
     while (true) {
         // Check if we've exhausted directory clusters (for non-fixed root)
         if (fctx->readdir_current_cluster >= fs->eoc_marker && !(fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0)) {
              ret = -FS_ERR_NOT_FOUND; // End of directory chain
              break;
         }
 
         uint32_t sec_size = fs->bytes_per_sector;
         uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
         size_t   offset_in_sector       = fctx->readdir_current_offset % sec_size;
         size_t   entries_per_sector     = sec_size / sizeof(fat_dir_entry_t);
         size_t   entry_index_in_sector  = offset_in_sector / sizeof(fat_dir_entry_t);
 
         // Read the current directory sector
         int read_res = read_directory_sector(fs, fctx->readdir_current_cluster,
                                              sector_offset_in_chain, sector_buffer);
         if (read_res != FS_SUCCESS) {
             ret = (read_res == -FS_ERR_IO) ? -FS_ERR_IO : -FS_ERR_NOT_FOUND; // End of dir or IO error
             break;
         }
 
         // Process entries within the sector
         for (size_t e_i = entry_index_in_sector; e_i < entries_per_sector; e_i++)
         {
             fat_dir_entry_t *dent = (fat_dir_entry_t*)(sector_buffer + e_i * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = fctx->readdir_current_offset; // Offset *before* incrementing
 
             // Advance the scan offset for the *next* iteration or exit
             fctx->readdir_current_offset += sizeof(fat_dir_entry_t);
 
             // Check entry type
             if (dent->name[0] == FAT_DIR_ENTRY_UNUSED) {
                 // End of directory marker (all subsequent entries are unused)
                 ret = -FS_ERR_NOT_FOUND;
                 goto readdir_done; // Use goto for cleaner exit from nested loops
             }
             if (dent->name[0] == FAT_DIR_ENTRY_DELETED || dent->name[0] == FAT_DIR_ENTRY_KANJI) {
                 // Skip deleted entries and KANJI escapes (0x05) for now
                 lfn_count = 0; // Reset LFN collector if sequence is broken
                 continue;
             }
             if ((dent->attr & FAT_ATTR_VOLUME_ID) && !(dent->attr & FAT_ATTR_LONG_NAME)) {
                  // Skip volume label entry (but not LFN entries)
                  lfn_count = 0;
                  continue;
             }
 
             // Process LFN or 8.3 entry
             if ((dent->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                 // --- LFN Entry ---
                 fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                 // Basic validation: Check sequence number consistency if needed
                 if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                     // Store LFN entry (typically stored in reverse order on disk)
                     // We might need to verify seq number order and checksum later
                     lfn_collector[lfn_count++] = *lfn_ent;
                 } else {
                     // LFN buffer overflow - discard collected LFNs for this entry
                     terminal_printf("[FAT readdir] Warning: LFN entry sequence exceeded buffer (%d entries).\n", FAT_MAX_LFN_ENTRIES);
                     lfn_count = 0;
                 }
                 continue; // Move to the next entry in the sector
             }
             else {
                 // --- 8.3 Entry ---
                 // This is a potential candidate entry to return
 
                 // Check if this is the logical entry the user requested
                 if (current_logical_index == entry_index) {
                     // Found the target entry
                     char final_name[FAT_MAX_LFN_CHARS]; // Use large buffer
                     final_name[0] = '\0';
 
                     // Attempt to reconstruct LFN if available and valid
                     if (lfn_count > 0) {
                         // Assuming fat_lfn.c provides LFN helpers
                         uint8_t expected_sum = fat_calculate_lfn_checksum(dent->name);
                         // LFN entries are stored last-to-first, so collector[0] has highest seq num
                         // Check checksum of the *last* LFN entry (which should be collector[0] if stored in reverse order)
                         if (lfn_collector[0].checksum == expected_sum) {
                              fat_reconstruct_lfn(lfn_collector, lfn_count, final_name, sizeof(final_name));
                         } else {
                             // Checksum mismatch, discard LFN
                             terminal_printf("[FAT readdir] LFN checksum mismatch for 8.3 name '%.11s'.\n", dent->name);
                             lfn_count = 0; // Invalidate LFN
                         }
                     }
 
                     // If no valid LFN, format the 8.3 name
                     if (final_name[0] == '\0') {
                         // Assuming fat_utils.c provides fat_format_short_name
                         fat_format_short_name(dent->name, final_name);
                     }
 
                     // Populate the VFS dirent structure
                     strncpy(d_entry_out->d_name, final_name, sizeof(d_entry_out->d_name) - 1);
                     d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0'; // Ensure null termination
 
                     d_entry_out->d_ino = fat_get_entry_cluster(dent); // Use cluster as inode number
                     d_entry_out->d_type = (dent->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG; // Set VFS type
 
                     // Update state for next call
                     fctx->readdir_last_index = entry_index;
                     ret = FS_SUCCESS;
                     goto readdir_done; // Successfully found and processed the entry
                 }
 
                 // If not the target index, increment our logical index count and reset LFN buffer
                 current_logical_index++;
                 lfn_count = 0;
             }
         } // End loop through entries in sector
 
         // --- Move to the next sector or cluster ---
         // Check if we need to advance to the next cluster
         if (!(fs->type != FAT_TYPE_FAT32 && fctx->readdir_current_cluster == 0) && // Not scanning FAT12/16 root
             (fctx->readdir_current_offset % fs->cluster_size_bytes == 0))          // Reached end of current cluster data
         {
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, fctx->readdir_current_cluster, &next_c);
             if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) {
                 ret = -FS_ERR_NOT_FOUND; // End of chain or error reading FAT
                 break; // Exit outer while loop
             }
             fctx->readdir_current_cluster = next_c;
             fctx->readdir_current_offset  = 0; // Reset offset for new cluster
         }
         // For FAT12/16 root, check if we exceeded root directory sector count (handled by read_directory_sector)
         // read_directory_sector will return error if offset goes beyond root dir bounds
 
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
 
     fat_dir_entry_t entry;
     // We don't need the LFN content, but lookup provides parent info
     uint32_t parent_dir_cluster, entry_offset;
     int find_res = fat_lookup_path(fs, path, &entry, NULL, 0,
                                     &parent_dir_cluster, &entry_offset);
 
     if (find_res != FS_SUCCESS) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return find_res; // e.g., -FS_ERR_NOT_FOUND
     }
 
     // Check if it's a directory - cannot unlink directories with this function
     if (entry.attr & FAT_ATTR_DIRECTORY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_IS_A_DIRECTORY; // Use rmdir instead
     }
 
     // Check if it's read-only
     if (entry.attr & FAT_ATTR_READ_ONLY) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return -FS_ERR_PERMISSION_DENIED;
     }
 
     // --- Free cluster chain ---
     uint32_t file_cluster = fat_get_entry_cluster(&entry);
     if (file_cluster >= 2) {
         // Assuming fat_alloc.c provides fat_free_cluster_chain
         int free_res = fat_free_cluster_chain(fs, file_cluster);
         if (free_res != FS_SUCCESS) {
             // Log error, but proceed to mark entry deleted anyway if possible
             terminal_printf("[FAT unlink] Warning: Error freeing cluster chain for '%s' (err %d).\n", path, free_res);
             // Do not abort unlink, try best effort to remove directory entry
         }
     }
 
     // --- Mark directory entries as deleted ---
     // This is tricky: we need to find the *start* of the LFN sequence + 8.3 entry.
     // fat_lookup_path only gives us the offset of the 8.3 entry.
     // A simple approach (potentially leaving orphaned LFNs if lookup is basic):
     // Mark only the 8.3 entry for now.
     // TODO: Implement backward scan from 8.3 entry to find LFN start and mark all.
     size_t num_entries_to_mark = 1; // Start with just the 8.3 entry
     uint32_t first_entry_offset = entry_offset; // Offset of the 8.3 entry
 
     // --- Refined approach (requires modification to lookup or a new helper) ---
     // Assuming fat_find_in_dir (declared in .h) can give us LFN start offset
     // uint32_t first_lfn_offset = entry_offset; // Default if no LFN
     // fat_find_in_dir(fs, parent_dir_cluster, /* need filename component */, &entry, NULL, 0, &entry_offset, &first_lfn_offset);
     // if (first_lfn_offset < entry_offset) { // Check if LFN exists before 8.3
     //     num_entries_to_mark = ((entry_offset - first_lfn_offset) / sizeof(fat_dir_entry_t)) + 1;
     //     first_entry_offset = first_lfn_offset;
     // }
     // --- End Refined ---
 
     int mark_res = mark_directory_entries_deleted(fs, parent_dir_cluster,
                                                 first_entry_offset,
                                                 num_entries_to_mark,
                                                 FAT_DIR_ENTRY_DELETED);
     if (mark_res != FS_SUCCESS) {
         terminal_printf("[FAT unlink] Error marking directory entry deleted for '%s' (err %d).\n", path, mark_res);
         // Rollback? Difficult. FAT is likely inconsistent now.
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return mark_res; // Report the directory update error
     }
 
     // --- Flush changes ---
     // FAT changes (from free_chain) and directory changes need flushing.
     // flush_fat_table(fs); // Should be marked dirty by free_chain/set_entry
     buffer_cache_sync_device(fs->disk_ptr->blk_dev.device_name); // Sync device
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     terminal_printf("[FAT unlink] Successfully unlinked '%s'.\n", path);
     return FS_SUCCESS;
 }
 
 
 /* --- Internal Helper Implementations --- */
 
 /**
  * @brief Looks up a single path component within a directory cluster.
  * (Implementation based on the loop inside original fat_lookup_path)
  */
 int fat_find_in_dir(fat_fs_t *fs,
                     uint32_t dir_cluster,
                     const char *component,
                     fat_dir_entry_t *entry_out,
                     char *lfn_out, size_t lfn_max_len,
                     uint32_t *entry_offset_in_dir_out,
                     uint32_t *first_lfn_offset_out) // Added LFN offset output
 {
     KERNEL_ASSERT(fs != NULL && component != NULL && entry_out != NULL && entry_offset_in_dir_out != NULL);
     // Assertion: Assumes caller holds fs->lock
 
     uint32_t current_cluster = dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
     uint32_t current_byte_offset = 0; // Tracks offset within the logical directory stream
 
     if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
     if (first_lfn_offset_out) *first_lfn_offset_out = (uint32_t)-1; // Init to invalid offset
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) return -FS_ERR_OUT_OF_MEMORY;
 
     fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
     int lfn_count = 0;
     uint32_t current_lfn_start_offset = (uint32_t)-1; // Track start offset of current LFN sequence
 
     while (true) {
          // Check if we've exhausted directory clusters (for non-fixed root)
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
              break; // End of directory chain
         }
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             kfree(sector_data);
             // Treat IO error or end-of-dir (NOT_FOUND from read_directory_sector if offset too large) as component not found
             return -FS_ERR_NOT_FOUND;
         }
 
         // Process entries within the sector
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
             // Check for end-of-directory marker
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                 kfree(sector_data);
                 return -FS_ERR_NOT_FOUND; // Component not found
             }
             // Skip deleted entries
             if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) {
                 lfn_count = 0; // Reset LFN state
                 current_lfn_start_offset = (uint32_t)-1;
                 continue;
             }
              // Skip volume label entry
             if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) {
                 lfn_count = 0;
                 current_lfn_start_offset = (uint32_t)-1;
                 continue;
             }
 
             // Handle LFN or 8.3 entry
             if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                 // LFN Entry - collect it
                 if (lfn_count == 0) { // Start of a new LFN sequence
                     current_lfn_start_offset = entry_abs_offset;
                 }
                 if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                     lfn_collector[lfn_count++] = *((fat_lfn_entry_t*)de);
                 } else {
                     lfn_count = 0; // Overflow, invalidate sequence
                     current_lfn_start_offset = (uint32_t)-1;
                 }
             } else {
                 // 8.3 Entry - check for match
                 bool match = false;
                 char reconstructed_lfn_buf[FAT_MAX_LFN_CHARS]; // Temp buffer for comparison
 
                 if (lfn_count > 0) {
                     // Compare with reconstructed LFN if available
                     uint8_t expected_sum = fat_calculate_lfn_checksum(de->name);
                     if (lfn_collector[0].checksum == expected_sum) {
                         fat_reconstruct_lfn(lfn_collector, lfn_count, reconstructed_lfn_buf, sizeof(reconstructed_lfn_buf));
                         // Assuming fat_utils.c provides comparison
                         if (fat_compare_lfn(component, reconstructed_lfn_buf) == 0) {
                             match = true;
                             if (lfn_out && lfn_max_len > 0) {
                                 strncpy(lfn_out, reconstructed_lfn_buf, lfn_max_len - 1);
                                 lfn_out[lfn_max_len - 1] = '\0';
                             }
                         }
                     } else {
                         lfn_count = 0; // Checksum failed, invalidate LFN sequence
                         current_lfn_start_offset = (uint32_t)-1;
                     }
                 }
 
                 // If LFN didn't match or wasn't present, try 8.3 comparison
                 if (!match) {
                      // Assuming fat_utils.c provides comparison
                     if (fat_compare_8_3(component, de->name) == 0) {
                         match = true;
                         if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0'; // No LFN for this match
                         current_lfn_start_offset = (uint32_t)-1; // No LFN associated
                     }
                 }
 
                 // If a match was found (either LFN or 8.3)
                 if (match) {
                     memcpy(entry_out, de, sizeof(fat_dir_entry_t));
                     *entry_offset_in_dir_out = entry_abs_offset;
                     if (first_lfn_offset_out) {
                         *first_lfn_offset_out = current_lfn_start_offset; // Store LFN start (or -1)
                     }
                     kfree(sector_data);
                     return FS_SUCCESS; // Found the component
                 }
 
                 // If no match, reset LFN state for the next entry
                 lfn_count = 0;
                 current_lfn_start_offset = (uint32_t)-1;
             }
         } // End loop through entries in sector
 
         // Advance to next sector or cluster
         current_byte_offset += fs->bytes_per_sector;
 
         if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
             if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) {
                  break; // End of chain or error
             }
             current_cluster = next_c;
             current_byte_offset = 0; // Reset logical offset for the new cluster
         }
         // FAT12/16 root dir end check is handled by read_directory_sector failure
     }
 
     kfree(sector_data);
     return -FS_ERR_NOT_FOUND; // Component not found after scanning whole directory
 }
 
 
 /**
  * @brief Resolves a full absolute path to its final directory entry.
  * (Implementation based on original fat_lookup_path, using fat_find_in_dir)
  */
 int fat_lookup_path(fat_fs_t *fs, const char *path,
                      fat_dir_entry_t *entry_out,
                      char *lfn_out, size_t lfn_max_len,
                      uint32_t *entry_dir_cluster_out,
                      uint32_t *entry_offset_in_dir_out)
 {
     KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && entry_dir_cluster_out != NULL && entry_offset_in_dir_out != NULL);
     // Assertion: Assumes caller holds fs->lock
 
     if (path[0] != '/') {
         return -FS_ERR_INVALID_PARAM; // Only absolute paths supported currently
     }
 
     // Handle root directory lookup separately
     if (strcmp(path, "/") == 0) {
         memset(entry_out, 0, sizeof(*entry_out));
         entry_out->attr = FAT_ATTR_DIRECTORY; // Root is always a directory
         *entry_offset_in_dir_out = 0; // No specific offset for root itself
 
         if (fs->type == FAT_TYPE_FAT32) {
             entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
             entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
             *entry_dir_cluster_out = 0; // Root has no parent dir cluster in this sense
         } else {
             // FAT12/16 Root Directory
             entry_out->first_cluster_low  = 0;
             entry_out->first_cluster_high = 0;
             *entry_dir_cluster_out = 0; // Represents fixed root area
         }
 
         if (lfn_out && lfn_max_len > 0) {
             strncpy(lfn_out, "/", lfn_max_len -1); // Simple name for root
             lfn_out[lfn_max_len - 1] = '\0';
         }
         return FS_SUCCESS;
     }
 
     // --- Tokenize the path ---
     // Use a copy because strtok modifies the string
     char *path_copy = kmalloc(strlen(path) + 1);
     if (!path_copy) return -FS_ERR_OUT_OF_MEMORY;
     strcpy(path_copy, path);
 
     char *saveptr; // For strtok_r if available, otherwise manage state carefully
     char *component = strtok(path_copy + 1, "/"); // Start after the initial '/'
     // TODO: Replace strtok with strtok_r or a custom non-modifying tokenizer if reentrancy needed
 
     // --- Path Traversal ---
     uint32_t current_dir_cluster; // Cluster of the directory being scanned
     fat_dir_entry_t current_entry; // Entry found for the *current* component
 
     // Start scan from root
     if (fs->type == FAT_TYPE_FAT32) {
         current_dir_cluster = fs->root_cluster;
     } else {
         current_dir_cluster = 0; // Marker for FAT12/16 root
     }
     // Initialize current_entry to represent root before starting loop
     memset(&current_entry, 0, sizeof(current_entry));
     current_entry.attr = FAT_ATTR_DIRECTORY;
     // Cluster info for root already set above based on FAT type
 
     uint32_t previous_dir_cluster = 0; // Store parent cluster for final result
 
     while (component != NULL) {
         if (strlen(component) == 0) { // Skip empty components (e.g., "//")
             component = strtok(NULL, "/");
             continue;
         }
 
         // --- Handle "." and ".." ---
         if (strcmp(component, ".") == 0) {
             // Stays in the current directory, no change needed
             component = strtok(NULL, "/");
             continue;
         }
         if (strcmp(component, "..") == 0) {
             // TODO: Implement ".." traversal by finding the parent directory entry
             // This requires reading the current directory's ".."" entry to get the parent's cluster.
             // For simplicity, let's return an error for now.
              terminal_printf("[FAT lookup] Error: '..' component not yet supported.\n");
              kfree(path_copy);
              return -FS_ERR_NOT_SUPPORTED;
              // component = strtok(NULL, "/");
              // continue;
         }
 
         // --- Find current component within current_dir_cluster ---
         previous_dir_cluster = current_dir_cluster; // Remember parent before searching
 
         // Use fat_find_in_dir helper
         uint32_t component_entry_offset; // Offset where component was found
         int find_comp_res = fat_find_in_dir(fs, current_dir_cluster, component,
                                             &current_entry, // Update current_entry with found component
                                             lfn_out, lfn_max_len, // Pass LFN buffer
                                             &component_entry_offset, NULL); // Don't need LFN start offset here
 
         if (find_comp_res != FS_SUCCESS) {
             // Component not found in current directory
             kfree(path_copy);
             return -FS_ERR_NOT_FOUND;
         }
 
         // Check if we are done (last component found)
         char* next_component = strtok(NULL, "/");
         if (next_component == NULL) {
             // This was the last component, we found it.
             memcpy(entry_out, &current_entry, sizeof(*entry_out));
             *entry_dir_cluster_out = previous_dir_cluster; // Parent cluster
             *entry_offset_in_dir_out = component_entry_offset; // Offset within parent
             kfree(path_copy);
             return FS_SUCCESS;
         }
 
         // Not the last component, so it *must* be a directory to continue
         if (!(current_entry.attr & FAT_ATTR_DIRECTORY)) {
             kfree(path_copy);
             return -FS_ERR_NOT_A_DIRECTORY;
         }
 
         // Update current_dir_cluster for the next iteration
         current_dir_cluster = fat_get_entry_cluster(&current_entry);
         if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0) {
              // Handle entering FAT12/16 root (shouldn't happen via non-".."?)
              terminal_printf("[FAT lookup] Warning: Traversed into FAT12/16 root unexpectedly.\n");
         }
         component = next_component; // Move to the next token
 
     } // End while(component != NULL)
 
     // Should not be reached if path was valid "/" or "/a/b" etc.
     // Might be reached for path "/" if tokenization was strange.
     kfree(path_copy);
     return -FS_ERR_INTERNAL; // Indicate an unexpected state
 }
 
 /**
  * @brief Reads a specific sector from a directory cluster chain.
  * (Implementation copied from user's previous fat_dir.c - seems reasonable)
  */
 int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                           uint32_t sector_offset_in_chain,
                           uint8_t* buffer)
 {
     KERNEL_ASSERT(fs != NULL && buffer != NULL);
     // Assertion: Assumes caller holds lock if needed
 
     if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         // FAT12/16 root directory (fixed location)
         if (sector_offset_in_chain >= fs->root_dir_sectors) {
             return -FS_ERR_NOT_FOUND; // Offset beyond root directory size
         }
         uint32_t lba = fs->root_dir_start_lba + sector_offset_in_chain;
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else if (cluster >= 2) {
         // Regular directory (cluster chain)
         uint32_t current_cluster = cluster;
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
 
         // Traverse the cluster chain to find the correct cluster
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
             uint32_t next_cluster;
             if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != FS_SUCCESS) {
                 return -FS_ERR_IO; // Error reading FAT
             }
             if (next_cluster >= fs->eoc_marker) {
                 return -FS_ERR_NOT_FOUND; // Offset beyond allocated chain
             }
             current_cluster = next_cluster;
         }
 
         // Calculate LBA of the target sector
         uint32_t lba = fat_cluster_to_lba(fs, current_cluster);
         if (lba == 0) return -FS_ERR_IO; // Should not happen for valid cluster >= 2
         lba += sector_in_final_cluster;
 
         // Read using buffer cache
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) return -FS_ERR_IO;
         memcpy(buffer, b->data, fs->bytes_per_sector);
         buffer_release(b);
         return FS_SUCCESS;
     } else {
         // Invalid starting cluster (0 or 1, not FAT12/16 root)
         return -FS_ERR_INVALID_PARAM;
     }
 }
 
 /**
  * @brief Updates an existing 8.3 directory entry on disk.
  * (Implementation copied from user's previous fat_dir.c - seems reasonable)
  */
 int update_directory_entry(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const fat_dir_entry_t *new_entry)
 {
     KERNEL_ASSERT(fs != NULL && new_entry != NULL);
     // Assertion: Assumes caller holds fs->lock
 
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT; // Should be set during mount
 
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
     KERNEL_ASSERT(offset_in_sector + sizeof(fat_dir_entry_t) <= sector_size); // Entry shouldn't cross sector boundary
 
     // --- Determine LBA ---
     uint32_t lba;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         // FAT12/16 root directory sector
          if (sector_offset_in_chain >= fs->root_dir_sectors) return -FS_ERR_INVALID_PARAM; // Invalid offset
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) {
         // Need to find the correct sector within the cluster chain
         uint32_t current_cluster = dir_cluster;
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
 
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
              uint32_t next_cluster;
              if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != FS_SUCCESS) return -FS_ERR_IO;
              if (next_cluster >= fs->eoc_marker) return -FS_ERR_INVALID_PARAM; // Offset beyond chain
              current_cluster = next_cluster;
         }
         lba = fat_cluster_to_lba(fs, current_cluster);
         if (lba == 0) return -FS_ERR_IO;
         lba += sector_in_final_cluster;
     } else {
         return -FS_ERR_INVALID_PARAM; // Invalid directory cluster
     }
 
     // --- Read-Modify-Write using Buffer Cache ---
     buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
     if (!b) return -FS_ERR_IO;
 
     // Modify the entry within the buffer
     memcpy(b->data + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
 
     // Mark dirty and release
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
     KERNEL_ASSERT(fs != NULL && num_entries > 0);
      // Assertion: Assumes caller holds fs->lock
 
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     int result = FS_SUCCESS;
     size_t entries_marked = 0;
     uint32_t current_offset = first_entry_offset;
 
     // This needs to handle crossing sector boundaries carefully
     while (entries_marked < num_entries) {
         uint32_t sector_offset_in_chain = current_offset / sector_size;
         size_t offset_in_sector = current_offset % sector_size;
 
         // Determine LBA for the current sector (similar logic to update_directory_entry)
         uint32_t lba;
          if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
              if (sector_offset_in_chain >= fs->root_dir_sectors) { result = -FS_ERR_INVALID_PARAM; break; }
              lba = fs->root_dir_start_lba + sector_offset_in_chain;
          } else if (dir_cluster >= 2) {
              uint32_t current_data_cluster = dir_cluster; // Start from the beginning of the dir chain
              uint32_t sectors_per_cluster = fs->sectors_per_cluster;
              uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
              uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
              for (uint32_t i = 0; i < cluster_hop_count; i++) {
                  uint32_t next_cluster;
                  if (fat_get_next_cluster(fs, current_data_cluster, &next_cluster) != FS_SUCCESS) { result = -FS_ERR_IO; goto mark_fail; }
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto mark_fail; } // Offset beyond chain
                  current_data_cluster = next_cluster;
              }
              lba = fat_cluster_to_lba(fs, current_data_cluster);
              if (lba == 0) { result = -FS_ERR_IO; break; }
              lba += sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break;
          }
 
         // Get buffer for the sector
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; }
 
         // Mark entries within this sector
         bool buffer_dirtied = false;
         while (entries_marked < num_entries && offset_in_sector < sector_size) {
             fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(b->data + offset_in_sector);
             // Check if entry is already unused? Optional safety check.
             // if(entry_ptr->name[0] != FAT_DIR_ENTRY_UNUSED) {
                 entry_ptr->name[0] = marker; // Mark the first byte
                 buffer_dirtied = true;
             // }
             offset_in_sector += sizeof(fat_dir_entry_t);
             current_offset   += sizeof(fat_dir_entry_t);
             entries_marked++;
         }
 
         // Release buffer (mark dirty only if changed)
         if (buffer_dirtied) {
             buffer_mark_dirty(b);
         }
         buffer_release(b);
 
         // Check for errors before continuing loop
         if (result != FS_SUCCESS) break;
 
     } // end while
 
 mark_fail:
     // Ensure subsequent sync operations write out changes made so far, even if error occurred.
     return result;
 }
 
 
 /**
  * @brief Writes one or more consecutive directory entries to disk.
  * (Based on user's previous more complex version, with fixes/completion)
  */
 int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries)
 {
     KERNEL_ASSERT(fs != NULL && entries_buf != NULL);
     if (num_entries == 0) return FS_SUCCESS; // Nothing to write
 
     size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
     size_t sector_size = fs->bytes_per_sector;
     if (sector_size == 0) return -FS_ERR_INVALID_FORMAT;
 
     const uint8_t *src_buf = (const uint8_t *)entries_buf;
     size_t bytes_written = 0;
     int result = FS_SUCCESS;
 
     // Loop through writing potentially multiple sectors
     while (bytes_written < total_bytes) {
         uint32_t current_abs_offset = dir_offset + (uint32_t)bytes_written;
         uint32_t sector_offset_in_chain = current_abs_offset / sector_size;
         size_t offset_in_sector = current_abs_offset % sector_size;
 
         // Determine LBA for the current sector (copied logic from mark_deleted)
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
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto write_fail; } // Offset beyond chain
                  current_data_cluster = next_cluster;
              }
              lba = fat_cluster_to_lba(fs, current_data_cluster);
               if (lba == 0) { result = -FS_ERR_IO; break; }
              lba += sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break;
          }
 
         // Get buffer for the sector (read-modify-write potentially needed)
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; }
 
         // Determine how many bytes to write into *this* sector
         size_t bytes_to_write_this_sector = sector_size - offset_in_sector;
         size_t bytes_remaining_total = total_bytes - bytes_written;
         if (bytes_to_write_this_sector > bytes_remaining_total) {
             bytes_to_write_this_sector = bytes_remaining_total;
         }
 
         // Copy data from source buffer to cache buffer
         memcpy(b->data + offset_in_sector, src_buf + bytes_written, bytes_to_write_this_sector);
 
         // Mark dirty and release
         buffer_mark_dirty(b);
         buffer_release(b);
 
         bytes_written += bytes_to_write_this_sector;
 
     } // end while
 
 write_fail:
     // Any error encountered will be in 'result'
     return result;
 }
 
 
 /**
  * @brief Finds a sequence of free slots in a directory.
  * (Basic implementation from fat.c, marked as placeholder - needs improvement)
  */
 int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                              size_t needed_slots,
                              uint32_t *out_slot_cluster,
                              uint32_t *out_slot_offset)
 {
     KERNEL_ASSERT(fs != NULL && needed_slots > 0 && out_slot_cluster != NULL && out_slot_offset != NULL);
     // Assertion: Assumes caller holds fs->lock
 
     // --- THIS IS A BASIC PLACEHOLDER IMPLEMENTATION ---
     // TODO: Needs proper handling of:
     // 1. Directory Extension: If no contiguous block found, allocate new cluster(s)
     //    for the directory file and link them.
     // 2. FAT12/16 Root Directory: Cannot be extended, has fixed size.
     // 3. Efficiency: Can be slow for large directories.
 
     terminal_printf("[FAT find_free_directory_slot] Warning: Using basic placeholder implementation.\n");
 
     uint32_t current_cluster = parent_dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && parent_dir_cluster == 0);
     uint32_t current_byte_offset = 0;
     size_t contiguous_free_count = 0;
     uint32_t potential_start_offset = 0;
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) return -FS_ERR_OUT_OF_MEMORY;
 
     while (true) {
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break; // End of chain
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             // End of fixed root dir, or IO error
             break;
         }
 
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED || de->name[0] == FAT_DIR_ENTRY_DELETED) {
                 if (contiguous_free_count == 0) {
                     potential_start_offset = entry_abs_offset; // Mark start of potential block
                 }
                 contiguous_free_count++;
                 if (contiguous_free_count >= needed_slots) {
                     // Found enough contiguous slots
                     *out_slot_cluster = current_cluster; // Cluster where the block starts/is located
                     *out_slot_offset = potential_start_offset;
                     kfree(sector_data);
                     return FS_SUCCESS;
                 }
             } else {
                 // Non-free entry breaks contiguity
                 contiguous_free_count = 0;
             }
 
              // Check for absolute end marker
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                  // If we reached here and haven't found enough slots, they don't exist contiguously
                  goto find_slot_fail;
             }
         }
 
         // Advance to next sector/cluster
          current_byte_offset += fs->bytes_per_sector;
          if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
              uint32_t next_c;
              int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
              if (get_next_res != FS_SUCCESS || next_c >= fs->eoc_marker) break; // End of chain or error
              current_cluster = next_c;
              current_byte_offset = 0;
          }
          // Fixed root end check handled by read_directory_sector failure
 
     } // End while
 
 find_slot_fail:
     kfree(sector_data);
     terminal_printf("[FAT find_free_directory_slot] No contiguous slot found (%u needed).\n", needed_slots);
     // Placeholder: return NO_SPACE. Real impl needs to try extending the directory.
     if (scanning_fixed_root) {
          terminal_printf("[FAT find_free_directory_slot] Cannot extend FAT12/16 root directory.\n");
     } else {
          // TODO: Attempt to allocate a new cluster for the directory 'parent_dir_cluster'
          // and link it, then potentially write into the start of that new cluster.
          terminal_printf("[FAT find_free_directory_slot] Directory extension logic not implemented.\n");
     }
     return -FS_ERR_NO_SPACE;
 }