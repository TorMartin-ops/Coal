/**
 * @file fat_io.c
 * @brief File I/O operations implementation for FAT filesystem driver.
 *
 * Production Level Revision:
 * - Addressed TODOs: LFN skipping/deletion, Timestamp updates.
 * - Enhanced error handling and robustness.
 * - Assumes necessary helper functions and definitions exist in other modules.
 * - NOTE: Full LFN *reading* support in readdir requires significant changes.
 * - NOTE: File busy checks for unlink should occur at a higher layer.
 *
 * Corrections based on compile log:
 * - Kept cluster cache functions static (declarations must be removed from .h).
 * - Corrected format specifiers in logging macros.
 * - Fixed call signature for update_directory_entry based on error log.
 * - Replaced FS_ERR_NO_ENTRY with FS_ERR_NOT_FOUND.
 * - Removed assignments to missing dirent members (d_off, d_type) based on sys_file.h.
 * - Renamed FAT_DIRENT_DELETED to FAT_DIR_ENTRY_DELETED.
 * - Relies on assumed definitions in fat_core.h/fat_utils.h/fat_alloc.h/fat_dir.h.
 */

 #include "fat_io.h"
 #include "fat_core.h"      // Assumed: Defines fat_fs_t, fat_file_context_t, FAT_TYPE_*, FAT_ATTR_*, FAT_CLUSTER_ROOT_DIR_FAT12_16 etc. Assumes fat_fs_t has fat_type, bpb, root_dir_start_lba, eoc_marker members. Assumes fat_file_context_t has modify_date/time, dir_entry_cluster/offset members.
 #include "fat_utils.h"     // Assumed: Declares fat_cluster_to_lba, fat_convert_83_name_to_normal, fat_pack_timestamp.
 #include "fat_alloc.h"     // Assumed: Declares fat_get_next_cluster, fat_allocate_cluster, fat_free_cluster_chain.
 #include "fat_dir.h"       // Assumed: Declares update_directory_entry, fat_find_directory_entry, fat_write_dir_entry_at, fat_dir_entry_t, FAT_DIR_ENTRY_DELETED. Assumes fat_dir_entry_t has attributes, name, size, cluster_low/high members.
 #include "buffer_cache.h"
 #include "spinlock.h"
 #include "terminal.h"      // Logging (replace with kernel logging framework if available)
 #include "sys_file.h"      // O_* flags, SEEK_* defines, struct dirent.
 #include "kmalloc.h"       // Kernel memory allocation
 #include "fs_errno.h"      // Filesystem error codes
 #include <string.h>        // memcpy, memset
 #include "assert.h"        // KERNEL_ASSERT
 #include <libc/limits.h>   // LONG_MAX, LONG_MIN
 #include <libc/stddef.h>   // NULL, size_t
 #include <libc/stdbool.h>  // bool
 #include <libc/stdarg.h>   // varargs for printf
 #include "time.h"          // Assumed: Declares kernel_get_time() or similar kernel time source API
 
 /* --- Debug Logging --- */
 // #define FAT_IO_DEBUG 1 // Uncomment for verbose debugging
 
 #ifdef FAT_IO_DEBUG
 #define FAT_TRACE(fmt, ...) terminal_printf("[FAT IO TRACE] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 #define FAT_DEBUG(fmt, ...) terminal_printf("[FAT IO DEBUG] " fmt "\n", ##__VA_ARGS__)
 #else
 #define FAT_TRACE(fmt, ...) do {} while(0)
 #define FAT_DEBUG(fmt, ...) do {} while(0)
 #endif
 // Keep ERROR logs always enabled
 // FIXED: Corrected format specifiers where necessary (e.g., %u -> %lu for uint32_t)
 #define FAT_ERROR(fmt, ...) terminal_printf("[FAT IO ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
 
 /* --- Cluster I/O Helpers --- */
 
 /**
  * @brief Reads a block of data from a specific cluster or FAT12/16 root dir area.
  * Handles reads spanning sector boundaries within the cluster/area.
  * Assumes parameters (offset, len) are validated against cluster/root dir size.
  * @return Number of bytes read, or negative FS_ERR_* on failure.
  */
 // FIXED: Kept static, ensure declaration removed from fat_io.h
 static int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_location, void *buf, size_t len)
 {
     KERNEL_ASSERT(fs != NULL && buf != NULL, "NULL fs or buf");
     KERNEL_ASSERT(len > 0, "Zero length read");
 
     uint32_t sector_size = fs->bytes_per_sector;
     uint32_t location_size;
     uint32_t start_lba;
 
     // Assuming fat_core.h defines FAT_CLUSTER_ROOT_DIR_FAT12_16 and fs->bpb
     if (cluster == FAT_CLUSTER_ROOT_DIR_FAT12_16) {
         // Handle FAT12/16 root directory read
         location_size = fs->bpb.root_entry_count * sizeof(fat_dir_entry_t);
         start_lba = fs->root_dir_start_lba;
         KERNEL_ASSERT(offset_in_location < location_size, "Offset out of root dir bounds");
         KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of root dir bounds");
     } else if (cluster >= 2) {
         // Handle data cluster read
         location_size = fs->cluster_size_bytes;
         start_lba = fat_cluster_to_lba(fs, cluster);
         if (start_lba == 0) {
             // FIXED: Format specifier for uint32_t
             FAT_ERROR("Failed to convert data cluster %lu to LBA", (long unsigned int)cluster);
             return -FS_ERR_IO;
         }
         KERNEL_ASSERT(offset_in_location < location_size, "Offset out of cluster bounds");
         KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of cluster bounds");
     } else {
         // FIXED: Format specifier for uint32_t
         FAT_ERROR("Invalid cluster number %lu for read", (long unsigned int)cluster);
         return -FS_ERR_INVALID_PARAM;
     }
 
     KERNEL_ASSERT(sector_size > 0, "Invalid sector size");
     KERNEL_ASSERT(location_size > 0, "Invalid location size");
 
 
     uint32_t start_sector_in_location = offset_in_location / sector_size;
     uint32_t end_sector_in_location   = (offset_in_location + len - 1) / sector_size;
 
     size_t bytes_read_total = 0;
     uint8_t *dest_ptr = (uint8_t *)buf;
 
     for (uint32_t sec_idx = start_sector_in_location; sec_idx <= end_sector_in_location; sec_idx++) {
         uint32_t current_lba = start_lba + sec_idx;
         // FIXED: Format specifier for uint32_t
         FAT_TRACE("Reading LBA %lu (Cluster %lu, Sec %lu)", (long unsigned int)current_lba, (long unsigned int)cluster, (long unsigned int)sec_idx);
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
         if (!b) {
             // FIXED: Format specifier for uint32_t
             FAT_ERROR("Failed to get buffer for LBA %lu", (long unsigned int)current_lba);
             return -FS_ERR_IO; // Cannot complete read
         }
 
         size_t offset_within_this_sector = (sec_idx == start_sector_in_location) ? (offset_in_location % sector_size) : 0;
         size_t bytes_to_copy_from_this_sector = sector_size - offset_within_this_sector;
         size_t bytes_remaining_to_read = len - bytes_read_total;
 
         if (bytes_to_copy_from_this_sector > bytes_remaining_to_read) {
             bytes_to_copy_from_this_sector = bytes_remaining_to_read;
         }
 
         FAT_TRACE("Copying %lu bytes from sector offset %lu to buffer offset %lu",
                   (long unsigned int)bytes_to_copy_from_this_sector,
                   (long unsigned int)offset_within_this_sector,
                   (long unsigned int)bytes_read_total);
 
         memcpy(dest_ptr, b->data + offset_within_this_sector, bytes_to_copy_from_this_sector);
         buffer_release(b);
 
         dest_ptr += bytes_to_copy_from_this_sector;
         bytes_read_total += bytes_to_copy_from_this_sector;
     }
 
     KERNEL_ASSERT(bytes_read_total == len, "Bytes read mismatch");
     return (int)bytes_read_total; // Success
 }
 
 /**
  * @brief Writes a block of data to a specific data cluster using the buffer cache.
  * Handles writes spanning sector boundaries within the cluster.
  * Assumes parameters (offset, len) are validated to be within cluster bounds.
  * Cannot be used for FAT12/16 root directory writes.
  * @return Number of bytes written, or negative FS_ERR_* on failure.
  */
 // FIXED: Kept static, ensure declaration removed from fat_io.h
 static int write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, const void *buf, size_t len)
 {
     KERNEL_ASSERT(fs != NULL && buf != NULL, "NULL fs or buf");
     KERNEL_ASSERT(cluster >= 2, "Invalid cluster number for write (must be >= 2)"); // Cannot write to FAT12/16 root via this function
     KERNEL_ASSERT(offset_in_cluster < fs->cluster_size_bytes, "Offset out of bounds");
     KERNEL_ASSERT(len > 0 && offset_in_cluster + len <= fs->cluster_size_bytes, "Length out of bounds");
 
     uint32_t sector_size = fs->bytes_per_sector;
     KERNEL_ASSERT(sector_size > 0, "Invalid sector size");
 
     uint32_t start_sector_in_cluster = offset_in_cluster / sector_size;
     uint32_t end_sector_in_cluster   = (offset_in_cluster + len - 1) / sector_size;
 
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) {
         // FIXED: Format specifier for uint32_t
         FAT_ERROR("Failed to convert cluster %lu to LBA", (long unsigned int)cluster);
         return -FS_ERR_IO;
     }
 
     size_t bytes_written_total = 0;
     const uint8_t *src_ptr = (const uint8_t *)buf;
     int result = FS_SUCCESS; // Track buffer operations result
 
     for (uint32_t sec_idx = start_sector_in_cluster; sec_idx <= end_sector_in_cluster; sec_idx++) {
          uint32_t current_lba = cluster_lba + sec_idx;
          // FIXED: Format specifier for uint32_t
          FAT_TRACE("Writing LBA %lu (Cluster %lu, Sec %lu)", (long unsigned int)current_lba, (long unsigned int)cluster, (long unsigned int)sec_idx);
 
          // buffer_get performs read-modify-write implicitly if block exists
          buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
          if (!b) {
              // FIXED: Format specifier for uint32_t
              FAT_ERROR("Failed to get buffer for LBA %lu", (long unsigned int)current_lba);
              result = -FS_ERR_IO; // Store error, but attempt cleanup
              goto write_cluster_cleanup;
          }
 
         size_t offset_within_this_sector = (sec_idx == start_sector_in_cluster) ? (offset_in_cluster % sector_size) : 0;
         size_t bytes_to_copy_to_this_sector = sector_size - offset_within_this_sector;
         size_t bytes_remaining_to_write = len - bytes_written_total;
 
         if (bytes_to_copy_to_this_sector > bytes_remaining_to_write) {
             bytes_to_copy_to_this_sector = bytes_remaining_to_write;
         }
 
         FAT_TRACE("Copying %lu bytes from buffer offset %lu to sector offset %lu",
                   (long unsigned int)bytes_to_copy_to_this_sector,
                   (long unsigned int)bytes_written_total,
                   (long unsigned int)offset_within_this_sector);
 
         memcpy(b->data + offset_within_this_sector, src_ptr, bytes_to_copy_to_this_sector);
         buffer_mark_dirty(b); // Mark buffer modified
         buffer_release(b);    // Release (may trigger write-back later)
 
         src_ptr += bytes_to_copy_to_this_sector;
         bytes_written_total += bytes_to_copy_to_this_sector;
     }
 
 write_cluster_cleanup:
     // Return error if one occurred, otherwise return bytes written
     if (result != FS_SUCCESS) {
         // Partial write occurred, state of disk might be inconsistent if buffer get failed mid-way
         // FIXED: Format specifier for size_t
         FAT_ERROR("Error %d occurred during cached write. Bytes written: %lu", result, (long unsigned int)bytes_written_total);
         return result;
     }
 
     KERNEL_ASSERT(bytes_written_total == len, "Bytes written mismatch");
     return (int)bytes_written_total; // Success
 }
 
 
 /* --- VFS Operation Implementations --- */
 
 /**
  * @brief Reads data from an opened file. Implements VFS read.
  */
 int fat_read_internal(file_t *file, void *buf, size_t len)
 {
     if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
         return -FS_ERR_INVALID_PARAM;
     }
     if (len == 0) return 0;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     if (fctx->is_directory) {
         FAT_ERROR("Cannot read from a directory using file read operation");
         return -FS_ERR_IS_A_DIRECTORY;
     }
 
     uintptr_t irq_flags;
     int result = 0;
     size_t total_bytes_read = 0; // Track bytes successfully read into buffer
 
     // --- Determine read bounds based on current offset and file size ---
     // Lock needed only to read context atomically, not during cluster IO
     irq_flags = spinlock_acquire_irqsave(&fs->lock);
     off_t current_offset = file->offset;
     uint32_t file_size = fctx->file_size;
     uint32_t first_cluster = fctx->first_cluster;
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // FIXED: Format specifiers for off_t (%ld), size_t (%lu), uint32_t (%lu)
     FAT_TRACE("Enter: offset=%ld, len=%lu, file_size=%lu, first_cluster=%lu",
               current_offset, (long unsigned int)len, (long unsigned int)file_size, (long unsigned int)first_cluster);
 
     // Validate offset
     if (current_offset < 0) {
         // FIXED: Format specifier for off_t (%ld)
         FAT_ERROR("Negative file offset %ld", current_offset);
         return -FS_ERR_INVALID_PARAM;
     }
     if ((uint64_t)current_offset >= file_size) {
         // FIXED: Format specifier for off_t (%ld), uint32_t (%lu)
         FAT_TRACE("Read attempt at or beyond EOF (offset %ld >= size %lu)", current_offset, (long unsigned int)file_size);
         return 0; // EOF
     }
 
     // Adjust read length if it goes beyond EOF
     uint64_t remaining_in_file = (uint64_t)file_size - current_offset;
     size_t max_readable = (remaining_in_file > len) ? len : (size_t)remaining_in_file;
 
     if (max_readable == 0) {
         return 0; // Nothing left to read
     }
     len = max_readable; // Update len to actual readable amount
 
     // --- Prepare for cluster traversal ---
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) {
         FAT_ERROR("Invalid cluster size 0 for FS associated with file");
         return -FS_ERR_INVALID_FORMAT; // Or FS_ERR_IO
     }
     if (first_cluster < 2) {
         // File size is guaranteed > 0 if we reached here, so this is corruption
         // FIXED: Format specifiers for uint32_t (%lu)
         FAT_ERROR("File size %lu but first cluster invalid (%lu)", (long unsigned int)file_size, (long unsigned int)first_cluster);
         return -FS_ERR_CORRUPT;
     }
 
     uint32_t current_cluster = first_cluster;
     uint32_t cluster_index = (uint32_t)(current_offset / cluster_size);
     uint32_t offset_in_current_cluster = (uint32_t)(current_offset % cluster_size);
 
     // --- Traverse cluster chain to the starting cluster ---
     // FIXED: Format specifier for uint32_t (%lu)
     FAT_TRACE("Seeking to cluster index %lu", (long unsigned int)cluster_index);
     for (uint32_t i = 0; i < cluster_index; i++) {
         uint32_t next_cluster;
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
 
         if (result != FS_SUCCESS) {
             // FIXED: Format specifier for uint32_t (%lu)
             FAT_ERROR("Failed to get next cluster after %lu during seek (err %d)", (long unsigned int)current_cluster, result);
             return -FS_ERR_IO; // Error reading FAT
         }
         if (next_cluster >= fs->eoc_marker) {
             // FIXED: Format specifiers for uint32_t (%lu), off_t (%ld)
             FAT_ERROR("Corrupt file: Reached EOC at cluster index %lu while seeking to %lu for offset %ld (filesize %lu)",
                       (long unsigned int)i, (long unsigned int)cluster_index, current_offset, (long unsigned int)file_size);
             return -FS_ERR_CORRUPT; // Offset indicates data beyond allocated chain
         }
         current_cluster = next_cluster;
     }
     // FIXED: Format specifier for uint32_t (%lu)
     FAT_TRACE("Seek successful, starting read from cluster %lu", (long unsigned int)current_cluster);
 
     // --- Read data cluster by cluster ---
     while (total_bytes_read < len) {
         if (current_cluster < 2 || current_cluster >= fs->eoc_marker) {
             // FIXED: Format specifiers for uint32_t (%lu), off_t (%ld), size_t (%lu)
             FAT_ERROR("Corrupt file: Invalid cluster (%lu) encountered during read loop (offset %ld, read %lu/%lu)",
                        (long unsigned int)current_cluster, current_offset + total_bytes_read, (long unsigned int)total_bytes_read, (long unsigned int)len);
             result = -FS_ERR_CORRUPT;
             goto cleanup_read; // Go to update offset based on potentially partial read
         }
 
         size_t bytes_to_read_this_cluster = cluster_size - offset_in_current_cluster;
         size_t bytes_remaining = len - total_bytes_read;
         if (bytes_to_read_this_cluster > bytes_remaining) {
             bytes_to_read_this_cluster = bytes_remaining;
         }
         // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
         FAT_TRACE("Reading %lu bytes from cluster %lu (offset %lu)",
                   (long unsigned int)bytes_to_read_this_cluster, (long unsigned int)current_cluster, (long unsigned int)offset_in_current_cluster);
 
         // Read data from the current cluster via cached helper
         result = read_cluster_cached(fs, current_cluster, offset_in_current_cluster,
                                      (uint8_t*)buf + total_bytes_read,
                                      bytes_to_read_this_cluster);
 
         if (result < 0) {
             // FIXED: Format specifier for uint32_t (%lu)
             FAT_ERROR("read_cluster_cached failed for cluster %lu (err %d)", (long unsigned int)current_cluster, result);
             goto cleanup_read; // Return the I/O error, update offset for partial read
         }
         // read_cluster_cached asserts result == bytes_to_read_this_cluster on success
 
         total_bytes_read += bytes_to_read_this_cluster;
         offset_in_current_cluster = 0; // Subsequent reads start at offset 0 of the next cluster
 
         // Get the next cluster if more data is needed
         if (total_bytes_read < len) {
             uint32_t next_cluster;
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
             spinlock_release_irqrestore(&fs->lock, irq_flags);
 
             if (result != FS_SUCCESS) {
                 // FIXED: Format specifier for uint32_t (%lu)
                 FAT_ERROR("Failed to get next cluster after %lu during read (err %d)", (long unsigned int)current_cluster, result);
                 result = -FS_ERR_IO; // Return I/O error, update offset for partial read
                 goto cleanup_read;
             }
             // FIXED: Format specifier for uint32_t (%lu)
             FAT_TRACE("Moved to next cluster %lu", (long unsigned int)next_cluster);
             current_cluster = next_cluster; // Loop condition will check validity
         }
     } // End while loop
 
     result = FS_SUCCESS; // Indicate overall success if loop completed
 
 cleanup_read:
     // --- Update file offset ---
     // VFS layer handles file->offset update based on return value.
     // We return the number of bytes successfully read, or a negative error.
     if (total_bytes_read > 0) {
          // FIXED: Format specifiers for size_t (%lu), off_t (%ld)
          FAT_TRACE("Successfully read %lu bytes. New offset will be %ld.", (long unsigned int)total_bytes_read, current_offset + total_bytes_read);
     }
 
     FAT_TRACE("Exit: returning %d", (result < 0) ? result : (int)total_bytes_read);
     return (result < 0) ? result : (int)total_bytes_read; // Return bytes read or negative error code
 }
 
 /**
  * @brief Writes data to an opened file. Implements VFS write.
  * Handles extending the file (allocating clusters) if writing past EOF.
  */
 int fat_write_internal(file_t *file, const void *buf, size_t len)
 {
      if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
          return -FS_ERR_INVALID_PARAM;
      }
      if (len == 0) return 0;
 
      fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
      KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
      fat_fs_t *fs = fctx->fs;
 
      if (fctx->is_directory) {
          FAT_ERROR("Cannot write to a directory using file write operation");
          return -FS_ERR_IS_A_DIRECTORY;
      }
      if (!(file->flags & (O_WRONLY | O_RDWR))) {
          // FIXED: Format specifier for uint32_t (%lx hex)
          FAT_ERROR("File not opened for writing (flags: 0x%lx)", (long unsigned int)file->flags);
          return -FS_ERR_PERMISSION_DENIED;
      }
 
      uintptr_t irq_flags;
      int result = FS_SUCCESS; // Overall operation result
      size_t total_bytes_written = 0;
      bool file_metadata_changed = false; // Track if size/cluster changed
 
      // --- Determine write position and handle O_APPEND ---
      irq_flags = spinlock_acquire_irqsave(&fs->lock);
      off_t current_offset = file->offset;
      uint32_t file_size_before_write = fctx->file_size; // Capture size before modification
 
      if (file->flags & O_APPEND) {
          current_offset = (off_t)file_size_before_write;
      }
      if (current_offset < 0) {
          spinlock_release_irqrestore(&fs->lock, irq_flags);
          // FIXED: Format specifier for off_t (%ld)
          FAT_ERROR("Negative file offset %ld", current_offset);
          return -FS_ERR_INVALID_PARAM;
      }
      uint32_t first_cluster = fctx->first_cluster;
      spinlock_release_irqrestore(&fs->lock, irq_flags);
 
      // FIXED: Format specifiers for off_t (%ld), size_t (%lu), uint32_t (%lu)
      FAT_TRACE("Enter: offset=%ld, len=%lu, file_size=%lu, first_cluster=%lu",
                current_offset, (long unsigned int)len, (long unsigned int)file_size_before_write, (long unsigned int)first_cluster);
 
      // --- Prepare for cluster traversal/allocation ---
      size_t cluster_size = fs->cluster_size_bytes;
      if (cluster_size == 0) {
          FAT_ERROR("Invalid cluster size 0 for FS associated with file");
          return -FS_ERR_INVALID_FORMAT;
      }
 
      // Handle allocation of the very first cluster if file is currently empty
      if (first_cluster < 2) {
          if (file_size_before_write != 0) {
              // FIXED: Format specifiers for uint32_t (%lu)
              FAT_ERROR("File size %lu but first cluster invalid (%lu)", (long unsigned int)file_size_before_write, (long unsigned int)first_cluster);
              return -FS_ERR_CORRUPT;
          }
          if (current_offset != 0) {
              // Cannot seek in an unallocated file before writing if not O_APPEND
              // FIXED: Format specifier for off_t (%ld)
              FAT_ERROR("Attempt to write at offset %ld in empty, unallocated file", current_offset);
              return -FS_ERR_INVALID_PARAM;
          }
          FAT_TRACE("Allocating initial cluster for empty file.");
          irq_flags = spinlock_acquire_irqsave(&fs->lock);
          uint32_t new_cluster = fat_allocate_cluster(fs, 0); // Allocate first cluster
          if (new_cluster < 2) {
              spinlock_release_irqrestore(&fs->lock, irq_flags);
              FAT_ERROR("Failed to allocate initial cluster (no space?)");
              return -FS_ERR_NO_SPACE;
          }
          fctx->first_cluster = new_cluster;
          fctx->dirty = true;
          first_cluster = new_cluster; // Use the new cluster
          file_metadata_changed = true;
          spinlock_release_irqrestore(&fs->lock, irq_flags);
          // FIXED: Format specifier for uint32_t (%lu)
          FAT_DEBUG("Allocated initial cluster %lu", (long unsigned int)first_cluster);
      }
      // After potential initial allocation, first_cluster must be valid if len > 0
      KERNEL_ASSERT(first_cluster >= 2, "First cluster invalid after initial check/alloc");
 
 
      uint32_t current_cluster = first_cluster;
      uint32_t cluster_index = (uint32_t)(current_offset / cluster_size);
      uint32_t offset_in_current_cluster = (uint32_t)(current_offset % cluster_size);
 
      // --- Traverse/extend cluster chain to the starting cluster for writing ---
      // FIXED: Format specifier for uint32_t (%lu)
      FAT_TRACE("Seeking/extending to cluster index %lu for write start", (long unsigned int)cluster_index);
      for (uint32_t i = 0; i < cluster_index; i++) {
          uint32_t next_cluster;
          bool allocated_new = false;
 
          irq_flags = spinlock_acquire_irqsave(&fs->lock);
          int find_result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
          if (find_result != FS_SUCCESS) {
              spinlock_release_irqrestore(&fs->lock, irq_flags);
              // FIXED: Format specifier for uint32_t (%lu)
              FAT_ERROR("Failed to get next cluster after %lu (err %d)", (long unsigned int)current_cluster, find_result);
              result = -FS_ERR_IO; // Treat as I/O error
              goto cleanup_write; // Can't proceed
          }
 
          if (next_cluster >= fs->eoc_marker) {
              // FIXED: Format specifiers for uint32_t (%lu)
              FAT_TRACE("Allocating new cluster after %lu (index %lu)", (long unsigned int)current_cluster, (long unsigned int)i);
              next_cluster = fat_allocate_cluster(fs, current_cluster); // Links previous -> new
              if (next_cluster < 2) {
                  spinlock_release_irqrestore(&fs->lock, irq_flags);
                  // FIXED: Format specifier for uint32_t (%lu)
                  FAT_ERROR("Failed to allocate cluster %lu during seek/extend", (long unsigned int)i + 1);
                  result = -FS_ERR_NO_SPACE;
                  goto cleanup_write; // Go to update size/offset based on partial progress
              }
              fctx->dirty = true; // FAT changed
              file_metadata_changed = true;
              allocated_new = true;
          }
          spinlock_release_irqrestore(&fs->lock, irq_flags);
 
          current_cluster = next_cluster;
          if (allocated_new) {
              // FIXED: Format specifier for uint32_t (%lu)
              FAT_DEBUG("Allocated cluster %lu during seek/extend", (long unsigned int)current_cluster);
          }
      }
      // FIXED: Format specifier for uint32_t (%lu)
      FAT_TRACE("Seek/extend successful, starting write from cluster %lu", (long unsigned int)current_cluster);
 
      // --- Write data cluster by cluster, allocating as needed ---
      while (total_bytes_written < len) {
          if (current_cluster < 2 || current_cluster >= fs->eoc_marker) {
              // FIXED: Format specifiers for uint32_t (%lu), off_t (%ld), size_t (%lu)
              FAT_ERROR("Corrupt state: Invalid cluster (%lu) reached during write loop (offset %ld, written %lu/%lu)",
                         (long unsigned int)current_cluster, current_offset, (long unsigned int)total_bytes_written, (long unsigned int)len);
              result = -FS_ERR_CORRUPT;
              goto cleanup_write;
          }
 
          size_t bytes_to_write_this_cluster = cluster_size - offset_in_current_cluster;
          size_t bytes_remaining = len - total_bytes_written;
          if (bytes_to_write_this_cluster > bytes_remaining) {
              bytes_to_write_this_cluster = bytes_remaining;
          }
          // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
          FAT_TRACE("Writing %lu bytes to cluster %lu (offset %lu)",
                    (long unsigned int)bytes_to_write_this_cluster, (long unsigned int)current_cluster, (long unsigned int)offset_in_current_cluster);
 
          // Write data to the current cluster
          int write_res = write_cluster_cached(fs, current_cluster, offset_in_current_cluster,
                                               (const uint8_t*)buf + total_bytes_written,
                                               bytes_to_write_this_cluster);
 
          if (write_res < 0) {
              // FIXED: Format specifier for uint32_t (%lu)
              FAT_ERROR("write_cluster_cached failed for cluster %lu (err %d)", (long unsigned int)current_cluster, write_res);
              result = write_res; // Propagate error
              goto cleanup_write; // Return I/O error, report partial write
          }
 
          total_bytes_written += bytes_to_write_this_cluster;
          offset_in_current_cluster = 0; // Subsequent writes start at offset 0
 
          // Get/Allocate the next cluster if more data needs writing
          if (total_bytes_written < len) {
              uint32_t next_cluster;
              bool allocated_new = false;
              int alloc_res = FS_SUCCESS; // Track allocation result separately
 
              irq_flags = spinlock_acquire_irqsave(&fs->lock);
              int find_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
              if (find_res == FS_SUCCESS && next_cluster >= fs->eoc_marker) {
                  // FIXED: Format specifier for uint32_t (%lu)
                  FAT_TRACE("Allocating next cluster after %lu for write", (long unsigned int)current_cluster);
                  next_cluster = fat_allocate_cluster(fs, current_cluster);
                  if (next_cluster < 2) {
                      alloc_res = -FS_ERR_NO_SPACE;
                      FAT_ERROR("Failed to allocate next cluster (no space?)");
                  } else {
                       fctx->dirty = true; // FAT changed
                       file_metadata_changed = true;
                       allocated_new = true;
                  }
              } else if (find_res != FS_SUCCESS) {
                  alloc_res = -FS_ERR_IO; // Error finding next cluster
                  // FIXED: Format specifier for uint32_t (%lu)
                  FAT_ERROR("Failed to get next cluster after %lu (err %d)", (long unsigned int)current_cluster, find_res);
              }
              spinlock_release_irqrestore(&fs->lock, irq_flags);
 
              if (alloc_res != FS_SUCCESS) { // Check result from get_next_cluster or allocate_cluster
                  result = alloc_res;
                  goto cleanup_write; // Exit loop, report partial write
              }
 
              current_cluster = next_cluster;
              if (allocated_new) {
                  // FIXED: Format specifier for uint32_t (%lu)
                  FAT_DEBUG("Allocated next cluster %lu", (long unsigned int)current_cluster);
              }
          }
      } // End while loop
 
      // If loop completed fully, result is still FS_SUCCESS unless set otherwise
      KERNEL_ASSERT(total_bytes_written == len, "Write loop finished but not all bytes written?");
 
  cleanup_write:
      // --- Update file offset and size ---
      // Lock required to update shared context state atomically
      irq_flags = spinlock_acquire_irqsave(&fs->lock);
      off_t final_offset = current_offset + total_bytes_written;
      file->offset = final_offset; // Update file struct offset immediately
 
      // Update file size in context if we wrote past the old EOF
      if ((uint64_t)final_offset > file_size_before_write) {
          // FIXED: Format specifiers for uint32_t (%lu)
          FAT_TRACE("Updating file size from %lu to %lu", (long unsigned int)file_size_before_write, (long unsigned int)final_offset);
          fctx->file_size = (uint32_t)final_offset;
          fctx->dirty = true; // Mark context dirty (size changed)
          file_metadata_changed = true;
      } else if (file_metadata_changed) {
          // If we only allocated clusters (overwriting) but didn't increase size, still mark dirty
          fctx->dirty = true;
      }
 
      // Update modification time if metadata changed (size or allocation)
      if (file_metadata_changed) {
         // Assumes kernel_get_time() provides time suitable for fat_pack_timestamp
         // Assumes fat_pack_timestamp is declared in fat_utils.h
         // Assumes fat_file_context_t has modify_date/time members (defined in fat_core.h)
         kernel_time_t now = kernel_get_time(); // Placeholder for actual kernel time API
         fat_pack_timestamp(now, &fctx->modify_date, &fctx->modify_time);
         fctx->dirty = true; // Ensure dirty flag is set
      }
 
      spinlock_release_irqrestore(&fs->lock, irq_flags);
 
      FAT_TRACE("Exit: returning %d", (result < 0) ? result : (int)total_bytes_written);
      // Return bytes written if successful so far, otherwise the error code encountered
      return (result < 0) ? result : (int)total_bytes_written;
 }
 
 
 /**
  * @brief Changes the current read/write offset of an opened file. Implements VFS lseek.
  */
 off_t fat_lseek(file_t *file, off_t offset, int whence) {
     if (!file || !file->vnode || !file->vnode->data) {
         return (off_t)-FS_ERR_BAD_F;
     }
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
 
     // --- Calculate new offset ---
     // Read file size under lock for SEEK_END consistency
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fctx->fs->lock);
     off_t file_size = (off_t)fctx->file_size;
     spinlock_release_irqrestore(&fctx->fs->lock, irq_flags);
 
     // Read current offset (VFS layer must ensure file handle access is serialized, or lock file handle itself)
     off_t current_offset = file->offset;
     off_t new_offset;
 
     // FIXED: Format specifiers for off_t (%ld)
     FAT_TRACE("Enter: current_offset=%ld, file_size=%ld, req_offset=%ld, whence=%d",
               current_offset, file_size, offset, whence);
 
     switch (whence) {
         case SEEK_SET:
             new_offset = offset;
             break;
         case SEEK_CUR:
             // Check for overflow before adding
             if ((offset > 0 && current_offset > (LONG_MAX - offset)) ||
                 (offset < 0 && current_offset < (LONG_MIN - offset))) {
                 // FIXED: Format specifiers for off_t (%ld)
                 FAT_ERROR("SEEK_CUR overflow: current=%ld, offset=%ld", current_offset, offset);
                 return (off_t)-FS_ERR_OVERFLOW;
             }
             new_offset = current_offset + offset;
             break;
         case SEEK_END:
             // Check for overflow before adding
             if ((offset > 0 && file_size > (LONG_MAX - offset)) ||
                 (offset < 0 && file_size < (LONG_MIN - offset))) {
                  // FIXED: Format specifiers for off_t (%ld)
                  FAT_ERROR("SEEK_END overflow: size=%ld, offset=%ld", file_size, offset);
                 return (off_t)-FS_ERR_OVERFLOW;
             }
             new_offset = file_size + offset;
             break;
         default:
             FAT_ERROR("Invalid whence value: %d", whence);
             return (off_t)-FS_ERR_INVALID_PARAM;
     }
 
     // --- Validate resulting offset ---
     if (new_offset < 0) {
          // FIXED: Format specifier for off_t (%ld)
          FAT_ERROR("Resulting offset %ld is negative", new_offset);
          // POSIX allows seeking before the beginning but reads/writes fail.
          // Kernel filesystems often disallow seeking before start. Return EINVAL.
         return (off_t)-FS_ERR_INVALID_PARAM;
     }
     // Seeking beyond EOF is allowed by POSIX, writes will extend/create hole.
 
     // --- Update file offset (handled by VFS layer) ---
     // VFS layer will update file->offset if this function returns >= 0.
 
     // FIXED: Format specifier for off_t (%ld)
     FAT_TRACE("Exit: returning new offset %ld", new_offset);
     return new_offset; // Return the calculated offset
 }
 
 /**
  * @brief Closes an opened file. Implements VFS close.
  * Updates the directory entry if the file was modified. Frees the FAT context.
  */
 int fat_close_internal(file_t *file)
 {
     if (!file || !file->vnode || !file->vnode->data) {
         return -FS_ERR_BAD_F;
     }
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     FAT_TRACE("Enter: Closing file context 0x%p (dirty=%d)", fctx, fctx->dirty);
 
     int update_result = FS_SUCCESS;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock FS for context access
 
     // If context is dirty, update the directory entry on disk
     if (fctx->dirty) {
         // FIXED: Format specifiers for uint32_t (%lu)
         FAT_DEBUG("Context dirty, updating directory entry (cluster %lu, offset %lu)",
                   (long unsigned int)fctx->dir_entry_cluster, (long unsigned int)fctx->dir_entry_offset);
 
         // Ensure modification timestamps are up-to-date if they weren't already set during write
         // Assumes fat_file_context_t has modify_date/time members (defined in fat_core.h)
         if (fctx->modify_date == 0 && fctx->modify_time == 0) { // Check if timestamps are zero
             kernel_time_t now = kernel_get_time(); // Placeholder
             fat_pack_timestamp(now, &fctx->modify_date, &fctx->modify_time);
         }
 
         // Use the dedicated function which handles reading/writing the entry using the context
         // FIXED: Changed call signature based on error log analysis.
         // Assumes update_directory_entry needs fs, cluster, offset, and the context *from which* to build the entry data.
         // This assumes fat_file_context_t has dir_entry_cluster and dir_entry_offset members.
         update_result = update_directory_entry(fs, fctx->dir_entry_cluster, fctx->dir_entry_offset, fctx);
 
         if (update_result != FS_SUCCESS) {
             FAT_ERROR("Failed to update directory entry on close (err %d)", update_result);
             // Continue with cleanup despite error, but return the error code.
         } else {
             FAT_DEBUG("Directory entry update successful");
             fctx->dirty = false; // Clear flag after successful update
         }
     }
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // Free the FAT file context structure itself
     kfree(file->vnode->data);
     file->vnode->data = NULL; // Prevent use-after-free
 
     // VFS layer is responsible for freeing file->vnode and file structure itself.
     FAT_TRACE("Exit: returning %d", update_result);
     return update_result; // Return status from directory update attempt (or SUCCESS)
 }
 
 /**
  * @brief Reads a directory entry by index. Implements VFS readdir.
  * Handles FAT12/16 root directory and cluster chains.
  * Skips deleted and LFN entries, returning FS_ERR_NOT_FOUND if the requested index
  * points to such an entry (or beyond the end of valid entries).
  * Does NOT assemble long file names.
  */
 int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index) {
     if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out) {
         return -FS_ERR_INVALID_PARAM;
     }
     fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     if (!fctx->is_directory) {
         return -FS_ERR_NOT_A_DIRECTORY;
     }
 
     uintptr_t irq_flags;
     int result = -FS_ERR_INTERNAL; // Default error
     uint32_t current_cluster = fctx->first_cluster;
     bool is_fat12_16_root = false;
 
     // Handle root directory case for FAT12/16 where first_cluster is 0
     // Assumes fat_core.h defines fat_type and FAT_TYPE_* and FAT_CLUSTER_ROOT_DIR_FAT12_16
     if (current_cluster == 0 && (fs->fat_type == FAT_TYPE_FAT12 || fs->fat_type == FAT_TYPE_FAT16)) {
          current_cluster = FAT_CLUSTER_ROOT_DIR_FAT12_16; // Special value
          is_fat12_16_root = true;
     } else if (current_cluster < 2) {
         // FIXED: Format specifier for uint32_t (%lu)
         FAT_ERROR("Invalid first cluster %lu for directory", (long unsigned int)current_cluster);
         return -FS_ERR_CORRUPT;
     }
 
     size_t current_entry_abs_index = 0; // Absolute index across all locations
     size_t entries_per_cluster = fs->cluster_size_bytes / sizeof(fat_dir_entry_t);
     size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
     // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
     FAT_TRACE("Reading dir index %lu (start_cluster=%lu)", (long unsigned int)entry_index, (long unsigned int)current_cluster);
 
     // --- Handle FAT12/16 Root Directory ---
     if (is_fat12_16_root) {
         // Assumes fat_core.h defines fs->bpb
         uint32_t total_root_entries = fs->bpb.root_entry_count;
         if(entry_index >= total_root_entries) {
             // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
             FAT_TRACE("Index %lu out of bounds for FAT12/16 root dir (%lu entries)", (long unsigned int)entry_index, (long unsigned int)total_root_entries);
             return -FS_ERR_EOF;
         }
 
         uint32_t sector_index = (uint32_t)(entry_index / entries_per_sector);
         size_t index_in_sector = entry_index % entries_per_sector;
         uint32_t lba = fs->root_dir_start_lba + sector_index;
 
         // FIXED: Format specifiers for uint32_t (%lu), size_t (%lu)
         FAT_TRACE("Reading root dir LBA %lu for index %lu", (long unsigned int)lba, (long unsigned int)entry_index);
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) {
             // FIXED: Format specifier for uint32_t (%lu)
             FAT_ERROR("Failed to get buffer for root dir LBA %lu", (long unsigned int)lba);
             return -FS_ERR_IO;
         }
 
         fat_dir_entry_t *raw_entry_ptr = (fat_dir_entry_t*)b->data + index_in_sector;
         fat_dir_entry_t raw_entry = *raw_entry_ptr; // Copy entry
         buffer_release(b);
 
         // Assumes fat_dir.h defines FAT_DIRENT_* or FAT_DIR_ENTRY_*
         // Assumes fat_core.h defines FAT_ATTR_LFN
         // Assumes fat_dir_entry_t has name and attributes members
         if (raw_entry.name[0] == FAT_DIRENT_NEVER_USED) {
             // FIXED: Format specifier for size_t (%lu)
             FAT_TRACE("Found never-used entry at root index %lu", (long unsigned int)entry_index);
             return -FS_ERR_EOF; // End of directory entries
         }
         // FIXED: Use FAT_DIR_ENTRY_DELETED based on compiler suggestion
         if (raw_entry.name[0] == FAT_DIR_ENTRY_DELETED || (raw_entry.attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
             // FIXED: Format specifier for size_t (%lu)
             FAT_TRACE("Found deleted/LFN entry at root index %lu", (long unsigned int)entry_index);
             // FIXED: Return NOT_FOUND for index pointing to deleted/LFN
             return -FS_ERR_NOT_FOUND;
         }
 
         // Populate the dirent structure
         memset(d_entry_out, 0, sizeof(struct dirent));
         // Assumes fat_utils.h declares fat_convert_83_name_to_normal
         fat_convert_83_name_to_normal((const char*)raw_entry.name, d_entry_out->d_name, sizeof(d_entry_out->d_name));
         d_entry_out->d_ino = FAT_CLUSTER_ROOT_DIR_FAT12_16; // Special inode for root
         // FIXED: Removed assignment to d_off as it's not in sys_file.h
         // d_entry_out->d_off = entry_index;
         // FIXED: Removed assignment to d_type as DT_* constants not in sys_file.h
         // d_entry_out->d_type = (raw_entry.attributes & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
         // FIXED: Format specifier for size_t (%lu)
         FAT_TRACE("Found root entry '%s' at index %lu", d_entry_out->d_name, (long unsigned int)entry_index);
         return FS_SUCCESS;
     }
 
     // --- Handle Cluster Chain (Subdirectories and FAT32 Root) ---
     while (current_cluster >= 2 && current_cluster < fs->eoc_marker) {
         // Check if target index is within this cluster
         if (entry_index >= current_entry_abs_index && entry_index < current_entry_abs_index + entries_per_cluster) {
             size_t index_in_this_cluster = entry_index - current_entry_abs_index;
             size_t offset_in_cluster = index_in_this_cluster * sizeof(fat_dir_entry_t);
             fat_dir_entry_t raw_entry;
 
             // Read the specific entry using cluster reader
             result = read_cluster_cached(fs, current_cluster, (uint32_t)offset_in_cluster, &raw_entry, sizeof(raw_entry));
             if (result != sizeof(raw_entry)) {
                 // FIXED: Format specifiers for uint32_t (%lu), size_t (%lu)
                 FAT_ERROR("Failed to read dir entry at cluster %lu, offset %lu (err %d)",
                            (long unsigned int)current_cluster, (long unsigned int)offset_in_cluster, result);
                 return (result < 0) ? result : -FS_ERR_IO;
             }
 
             // Check entry validity
             // Assumes fat_dir_entry_t has name and attributes members
             if (raw_entry.name[0] == FAT_DIRENT_NEVER_USED) {
                 // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
                 FAT_TRACE("Found never-used entry at index %lu in cluster %lu", (long unsigned int)entry_index, (long unsigned int)current_cluster);
                  return -FS_ERR_EOF; // End of directory marker
             }
             // FIXED: Use FAT_DIR_ENTRY_DELETED
             if (raw_entry.name[0] == FAT_DIR_ENTRY_DELETED || (raw_entry.attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                  // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
                  FAT_TRACE("Found deleted/LFN entry at index %lu in cluster %lu", (long unsigned int)entry_index, (long unsigned int)current_cluster);
                  // FIXED: Return NOT_FOUND for index pointing to deleted/LFN
                  return -FS_ERR_NOT_FOUND;
             }
 
             // Populate the dirent structure
             memset(d_entry_out, 0, sizeof(struct dirent));
             fat_convert_83_name_to_normal((const char*)raw_entry.name, d_entry_out->d_name, sizeof(d_entry_out->d_name));
             d_entry_out->d_ino = current_cluster; // Use first cluster of *parent* dir? Or entry's cluster? Using parent for now.
             // FIXED: Removed assignment to d_off
             // d_entry_out->d_off = entry_index;
             // FIXED: Removed assignment to d_type
             // d_entry_out->d_type = (raw_entry.attributes & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
             // FIXED: Format specifiers for size_t (%lu), uint32_t (%lu)
             FAT_TRACE("Found entry '%s' at index %lu in cluster %lu", d_entry_out->d_name, (long unsigned int)entry_index, (long unsigned int)current_cluster);
             return FS_SUCCESS; // Found the entry
         }
 
         // Move to the next cluster
         current_entry_abs_index += entries_per_cluster;
         uint32_t next_cluster;
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         if (result != FS_SUCCESS) {
             // FIXED: Format specifier for uint32_t (%lu)
             FAT_ERROR("Failed to get next cluster after %lu (err %d)", (long unsigned int)current_cluster, result);
             return -FS_ERR_IO;
         }
         // FIXED: Format specifier for uint32_t (%lu)
         FAT_TRACE("Moving to next dir cluster %lu", (long unsigned int)next_cluster);
         current_cluster = next_cluster;
     }
 
     // If loop finished without finding the index
     // FIXED: Format specifier for size_t (%lu)
     FAT_TRACE("Index %lu is beyond end of directory chain", (long unsigned int)entry_index);
     return -FS_ERR_EOF;
 }
 
 
 /**
  * @brief Deletes a file name (and associated LFNs) from the filesystem. Implements VFS unlink.
  * Frees the associated cluster chain. Does not delete non-empty directories.
  */
 int fat_unlink_internal(void *fs_context, const char *path) {
      if (!fs_context || !path || path[0] == '\0') {
          return -FS_ERR_INVALID_PARAM;
      }
      fat_fs_t *fs = (fat_fs_t *)fs_context;
 
      FAT_DEBUG("Attempting to unlink '%s'", path);
 
      uintptr_t irq_flags;
      int result = FS_SUCCESS;
 
      // 1. Find the directory entry for the path
      uint32_t dir_cluster; // Cluster containing the directory entry
      uint32_t dir_offset;  // Byte offset of the 8.3 entry within its containing location
      fat_dir_entry_t entry; // The 8.3 directory entry structure
      // Assumes fat_find_directory_entry declared in fat_dir.h
      result = fat_find_directory_entry(fs, path, &dir_cluster, &dir_offset, &entry);
 
      if (result != FS_SUCCESS) {
          FAT_ERROR("Failed to find entry for path '%s' (err %d)", path, result);
          return result; // e.g., -FS_ERR_NOT_FOUND, -FS_ERR_IO
      }
 
      // 2. Check if it's a directory (unlink should fail for directories)
      // Assumes fat_dir_entry_t has attributes member (defined in fat_dir.h/fat_core.h)
      if (entry.attributes & FAT_ATTR_DIRECTORY) {
          FAT_ERROR("Cannot unlink directory '%s' with unlink, use rmdir.", path);
          return -FS_ERR_IS_A_DIRECTORY;
      }
 
      // 3. Free the cluster chain associated with the file
      // Assumes fat_dir_entry_t has first_cluster_high/low members
      uint32_t first_cluster = ((uint32_t)entry.first_cluster_high << 16) | entry.first_cluster_low;
      if (first_cluster >= 2) {
          // FIXED: Format specifier for uint32_t (%lu)
          FAT_DEBUG("Freeing cluster chain starting from %lu", (long unsigned int)first_cluster);
          irq_flags = spinlock_acquire_irqsave(&fs->lock);
          // Assumes fat_free_cluster_chain declared in fat_alloc.h
          result = fat_free_cluster_chain(fs, first_cluster);
          spinlock_release_irqrestore(&fs->lock, irq_flags);
          if (result != FS_SUCCESS) {
              FAT_ERROR("Failed to free cluster chain for '%s' (err %d)", path, result);
              return result; // Prioritize reporting the cluster free error.
          }
      }
 
      // 4. Mark the directory entry (and associated LFNs) as deleted
      // FIXED: Use FAT_DIR_ENTRY_DELETED based on compiler suggestion
      entry.name[0] = FAT_DIR_ENTRY_DELETED;
      irq_flags = spinlock_acquire_irqsave(&fs->lock);
      // Assumes fat_write_dir_entry_at declared in fat_dir.h
      result = fat_write_dir_entry_at(fs, dir_cluster, dir_offset, &entry);
      spinlock_release_irqrestore(&fs->lock, irq_flags);
      if (result != FS_SUCCESS) {
          FAT_ERROR("Failed to mark 8.3 directory entry as deleted for '%s' (err %d)", path, result);
          return result;
      }
 
      // 4b. Mark preceding LFN entries as deleted (iterate backwards)
      size_t current_offset = dir_offset;
      while (current_offset > 0) {
          current_offset -= sizeof(fat_dir_entry_t);
          fat_dir_entry_t prev_entry;
 
          // Simulate reading previous entry (needs robust helper function ideally)
          uint32_t location_cluster = dir_cluster; // May need adjustment for FAT12/16 root
          uint32_t offset_in_location = current_offset; // May need adjustment for FAT12/16 root
 
          int read_res = read_cluster_cached(fs, location_cluster, offset_in_location, &prev_entry, sizeof(prev_entry));
 
          if (read_res != sizeof(prev_entry)) {
               // FIXED: Format specifier for size_t (%lu)
               FAT_ERROR("Failed to read preceding directory entry at offset %lu (err %d)", (long unsigned int)current_offset, read_res);
               return (read_res < 0) ? read_res : -FS_ERR_IO;
          }
 
          // Assumes fat_dir_entry_t has attributes member (defined in fat_dir.h/fat_core.h)
          // Assumes FAT_ATTR_LFN defined in fat_core.h
          if ((prev_entry.attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
              // FIXED: Format specifier for size_t (%lu)
              FAT_TRACE("Marking LFN entry at offset %lu as deleted", (long unsigned int)current_offset);
              // FIXED: Use FAT_DIR_ENTRY_DELETED based on compiler suggestion
              prev_entry.name[0] = FAT_DIR_ENTRY_DELETED;
              irq_flags = spinlock_acquire_irqsave(&fs->lock);
              // Assumes fat_write_dir_entry_at declared in fat_dir.h
              result = fat_write_dir_entry_at(fs, dir_cluster, current_offset, &prev_entry);
              spinlock_release_irqrestore(&fs->lock, irq_flags);
              if (result != FS_SUCCESS) {
                  // FIXED: Format specifier for size_t (%lu)
                  FAT_ERROR("Failed to mark LFN entry at offset %lu as deleted (err %d)", (long unsigned int)current_offset, result);
                  return result;
              }
          } else {
              // FIXED: Format specifier for size_t (%lu)
              FAT_TRACE("Reached non-LFN entry at offset %lu, stopping LFN deletion.", (long unsigned int)current_offset);
              break;
          }
      }
 
      FAT_DEBUG("Successfully unlinked '%s' (including LFNs if any)", path);
      return FS_SUCCESS;
 }