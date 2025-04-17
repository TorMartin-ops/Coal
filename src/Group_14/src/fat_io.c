/**
 * @file fat_io.c
 * @brief File I/O operations implementation for FAT filesystem driver.
 *
 * Implements VFS read, write, lseek, and close operations for FAT files.
 */

 #include "fat_io.h"     // Our declarations
 #include "fat_core.h"   // Core FAT structures
 #include "fat_utils.h"  // fat_get_next_cluster, fat_cluster_to_lba, etc.
 #include "fat_alloc.h"  // fat_allocate_cluster
 #include "fat_dir.h"    // update_directory_entry (used by close)
 #include "buffer_cache.h"// Buffer cache access
 #include "spinlock.h"   // Locking primitives
 #include "terminal.h"   // Logging
 #include "sys_file.h"   // O_* flags definitions
 #include "kmalloc.h"    // Kernel memory allocation
 #include "fs_errno.h"   // Filesystem error codes
 #include <string.h>     // memcpy, memset
 #include "assert.h"     // KERNEL_ASSERT
 #include <libc/limits.h> // For LONG_MAX, LONG_MIN, SIZE_MAX
 
 /* --- Cluster I/O Helpers --- */
 
 /**
  * @brief Reads a block of data from a cluster using buffer cache.
  * (Based on user's previous implementation)
  */
 int read_cluster_cached(fat_fs_t *fs, uint32_t cluster,
                         uint32_t offset_in_cluster,
                         void *buf, size_t len)
 {
     KERNEL_ASSERT(fs != NULL && buf != NULL);
     if (cluster < 2) {
         terminal_printf("[FAT Read Cluster] Error: Attempt to read invalid cluster %u\n", cluster);
         return -FS_ERR_INVALID_PARAM;
     }
      if (fs->cluster_size_bytes == 0 || fs->bytes_per_sector == 0) {
         terminal_printf("[FAT Read Cluster] Error: Invalid FS geometry (cluster size %u, sector size %u)\n",
                          fs->cluster_size_bytes, fs->bytes_per_sector);
         return -FS_ERR_INVALID_FORMAT;
     }
     if (offset_in_cluster >= fs->cluster_size_bytes || len > fs->cluster_size_bytes - offset_in_cluster) {
         terminal_printf("[FAT Read Cluster] Error: Invalid read params (offset=%u, len=%u, cluster_size=%u)\n",
                          offset_in_cluster, len, fs->cluster_size_bytes);
         return -FS_ERR_INVALID_PARAM; // Read would go past cluster boundary
     }
     if (len == 0) return 0; // Nothing to read
 
     uint32_t sector_size = fs->bytes_per_sector;
     uint32_t start_sector_in_cluster = offset_in_cluster / sector_size;
     uint32_t end_sector_in_cluster   = (offset_in_cluster + len - 1) / sector_size;
 
     // Calculate base LBA for the cluster
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) {
         terminal_printf("[FAT Read Cluster] Error: Failed to convert cluster %u to LBA.\n", cluster);
         return -FS_ERR_IO; // Or internal error
     }
 
     size_t bytes_read_total = 0;
     uint8_t *dest_ptr = (uint8_t *)buf;
 
     // Read sector by sector via buffer cache
     for (uint32_t sec_idx = start_sector_in_cluster; sec_idx <= end_sector_in_cluster; sec_idx++) {
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, cluster_lba + sec_idx);
         if (!b) {
             terminal_printf("[FAT Read Cluster] Error: Failed to get buffer for LBA %u (Cluster %u, Sector %u)\n",
                              cluster_lba + sec_idx, cluster, sec_idx);
             return -FS_ERR_IO; // Return error, partial read data in buf is undefined
         }
 
         // Calculate copy parameters for this sector
         size_t offset_within_this_sector = (sec_idx == start_sector_in_cluster) ? (offset_in_cluster % sector_size) : 0;
         size_t bytes_to_copy_from_this_sector = sector_size - offset_within_this_sector;
         size_t bytes_remaining_to_read = len - bytes_read_total;
 
         if (bytes_to_copy_from_this_sector > bytes_remaining_to_read) {
             bytes_to_copy_from_this_sector = bytes_remaining_to_read;
         }
 
         // Copy data from buffer cache block to destination buffer
         memcpy(dest_ptr, b->data + offset_within_this_sector, bytes_to_copy_from_this_sector);
 
         buffer_release(b); // Release the buffer cache block
 
         dest_ptr += bytes_to_copy_from_this_sector;
         bytes_read_total += bytes_to_copy_from_this_sector;
     }
 
     KERNEL_ASSERT(bytes_read_total == len); // Should have read exactly len bytes if parameters were valid
     return (int)bytes_read_total;
 }
 
 /**
  * @brief Writes a block of data to a cluster using buffer cache.
  * (Based on user's previous implementation)
  */
 int write_cluster_cached(fat_fs_t *fs, uint32_t cluster,
                          uint32_t offset_in_cluster,
                          const void *buf, size_t len)
 {
     KERNEL_ASSERT(fs != NULL && buf != NULL);
      if (cluster < 2) {
         terminal_printf("[FAT Write Cluster] Error: Attempt to write invalid cluster %u\n", cluster);
         return -FS_ERR_INVALID_PARAM;
     }
      if (fs->cluster_size_bytes == 0 || fs->bytes_per_sector == 0) {
          terminal_printf("[FAT Write Cluster] Error: Invalid FS geometry (cluster size %u, sector size %u)\n",
                           fs->cluster_size_bytes, fs->bytes_per_sector);
          return -FS_ERR_INVALID_FORMAT;
      }
     if (offset_in_cluster >= fs->cluster_size_bytes || len > fs->cluster_size_bytes - offset_in_cluster) {
         terminal_printf("[FAT Write Cluster] Error: Invalid write params (offset=%u, len=%u, cluster_size=%u)\n",
                          offset_in_cluster, len, fs->cluster_size_bytes);
         return -FS_ERR_INVALID_PARAM; // Write would go past cluster boundary
     }
      if (len == 0) return 0; // Nothing to write
 
     uint32_t sector_size = fs->bytes_per_sector;
     uint32_t start_sector_in_cluster = offset_in_cluster / sector_size;
     uint32_t end_sector_in_cluster   = (offset_in_cluster + len - 1) / sector_size;
 
     // Calculate base LBA for the cluster
     uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
     if (cluster_lba == 0) {
          terminal_printf("[FAT Write Cluster] Error: Failed to convert cluster %u to LBA.\n", cluster);
          return -FS_ERR_IO;
     }
 
     size_t bytes_written_total = 0;
     const uint8_t *src_ptr = (const uint8_t *)buf;
 
     // Write sector by sector via buffer cache (read-modify-write happens implicitly in buffer_get)
     for (uint32_t sec_idx = start_sector_in_cluster; sec_idx <= end_sector_in_cluster; sec_idx++) {
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, cluster_lba + sec_idx);
         if (!b) {
             terminal_printf("[FAT Write Cluster] Error: Failed to get buffer for LBA %u (Cluster %u, Sector %u)\n",
                              cluster_lba + sec_idx, cluster, sec_idx);
             return -FS_ERR_IO; // Return error, state of previously written sectors is undefined for caller
         }
 
         // Calculate copy parameters for this sector
         size_t offset_within_this_sector = (sec_idx == start_sector_in_cluster) ? (offset_in_cluster % sector_size) : 0;
         size_t bytes_to_copy_to_this_sector = sector_size - offset_within_this_sector;
         size_t bytes_remaining_to_write = len - bytes_written_total;
 
         if (bytes_to_copy_to_this_sector > bytes_remaining_to_write) {
             bytes_to_copy_to_this_sector = bytes_remaining_to_write;
         }
 
         // Copy data from source buffer to the buffer cache block
         memcpy(b->data + offset_within_this_sector, src_ptr, bytes_to_copy_to_this_sector);
 
         buffer_mark_dirty(b); // Mark the buffer as modified
         buffer_release(b);    // Release the buffer cache block
 
         src_ptr += bytes_to_copy_to_this_sector;
         bytes_written_total += bytes_to_copy_to_this_sector;
     }
 
     KERNEL_ASSERT(bytes_written_total == len); // Should have written exactly len bytes
     return (int)bytes_written_total;
 }
 
 
 /* --- VFS Operation Implementations --- */
 
 /**
  * @brief Reads data from an opened file.
  */
 int fat_read_internal(file_t *file, void *buf, size_t len)
 {
     if (!file || !file->vnode || !file->vnode->data || !buf) {
         return -FS_ERR_INVALID_PARAM;
     }
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL); // Context should always have valid fs pointer
     fat_fs_t *fs = fctx->fs;
 
     if (fctx->is_directory) {
         return -FS_ERR_IS_A_DIRECTORY;
     }
     if (len == 0) return 0;
 
     // --- Check bounds based on file size (needs lock for consistency) ---
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     uint32_t file_size = fctx->file_size; // Read size while holding lock
     off_t current_offset = file->offset; // Read offset while holding lock? Not strictly necessary if only this thread modifies it.
 
     if (current_offset < 0) {
         current_offset = 0; // Correct negative offset
         file->offset = 0;
     }
 
     // Check if already at or past EOF
     if ((uint64_t)current_offset >= file_size) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return 0; // EOF
     }
 
     // Adjust read length if it goes beyond EOF
     size_t max_readable = file_size - (size_t)current_offset;
     if (len > max_readable) {
         len = max_readable; // Clamp length to remaining bytes
     }
     if (len == 0) { // Can happen if offset was exactly file_size after correction
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         return 0;
     }
 
     uint32_t first_cluster = fctx->first_cluster; // Read first cluster while holding lock
     spinlock_release_irqrestore(&fs->lock, irq_flags); // --- Release lock before I/O ---
 
 
     // --- Prepare for cluster traversal ---
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) {
          terminal_printf("[FAT Read] Error: Invalid cluster size 0.\n");
          return -FS_ERR_INVALID_FORMAT;
     }
     if (first_cluster < 2 && file_size > 0) { // Check consistency
          terminal_printf("[FAT Read] Error: File size > 0 but first cluster invalid (%u).\n", first_cluster);
          return -FS_ERR_CORRUPT;
     }
      if (first_cluster < 2 && file_size == 0) {
          return 0; // Valid empty file, nothing to read
      }
 
     size_t total_bytes_read = 0;
     size_t bytes_left_to_read = len;
 
     // Calculate starting position
     uint32_t start_cluster_index = (uint32_t)(current_offset / cluster_size);
     uint32_t start_offset_in_cluster = (uint32_t)(current_offset % cluster_size);
     uint32_t current_cluster = first_cluster;
 
     // --- Traverse cluster chain to the starting cluster ---
     for (uint32_t i = 0; i < start_cluster_index; i++) {
         uint32_t next_cluster;
         // Need lock to safely read FAT table via helper
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
 
         if (get_next_res != FS_SUCCESS) {
             terminal_printf("[FAT Read] Error: Failed to get next cluster after %u (err %d).\n", current_cluster, get_next_res);
             return -FS_ERR_IO; // Error reading FAT
         }
         if (next_cluster >= fs->eoc_marker) {
             terminal_printf("[FAT Read] Error: Reached EOC prematurely while seeking to offset %ld (cluster index %u).\n", current_offset, i + 1);
             return -FS_ERR_CORRUPT; // Offset indicates data beyond allocated chain
         }
         current_cluster = next_cluster;
     }
 
     // --- Read data cluster by cluster ---
     while (bytes_left_to_read > 0 && current_cluster >= 2 && current_cluster < fs->eoc_marker)
     {
         size_t bytes_to_read_this_cluster = cluster_size - start_offset_in_cluster;
         if (bytes_to_read_this_cluster > bytes_left_to_read) {
             bytes_to_read_this_cluster = bytes_left_to_read;
         }
 
         // Read data from the current cluster via cached helper (no lock needed here)
         int rc = read_cluster_cached(fs, current_cluster, start_offset_in_cluster,
                                      (uint8_t*)buf + total_bytes_read,
                                      bytes_to_read_this_cluster);
 
         if (rc < 0) {
              terminal_printf("[FAT Read] Error: Failed to read %u bytes from cluster %u (err %d).\n", bytes_to_read_this_cluster, current_cluster, rc);
             // Update file offset based on potentially partial read before error? Difficult.
             // Let's return the error code directly. The actual bytes read might be partial.
             return rc;
         }
         if ((size_t)rc != bytes_to_read_this_cluster) {
             // Should not happen if read_cluster_cached works correctly and params were valid
             terminal_printf("[FAT Read] Warning: read_cluster_cached returned %d bytes, expected %u.\n", rc, bytes_to_read_this_cluster);
             // Treat as partial read completed
             total_bytes_read += rc;
             break; // Stop reading
         }
 
         total_bytes_read += bytes_to_read_this_cluster;
         bytes_left_to_read -= bytes_to_read_this_cluster;
         start_offset_in_cluster = 0; // Subsequent reads start at offset 0 of the next cluster
 
         // Get the next cluster if more data is needed
         if (bytes_left_to_read > 0) {
             uint32_t next_cluster;
             // Need lock to safely read FAT table
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
             spinlock_release_irqrestore(&fs->lock, irq_flags);
 
             if (get_next_res != FS_SUCCESS) {
                  terminal_printf("[FAT Read] Error: Failed to get next cluster after %u (err %d) during read.\n", current_cluster, get_next_res);
                  // Partial read completed, return bytes read so far. File offset is updated below.
                  break;
             }
             current_cluster = next_cluster; // Can be EOC marker, loop condition will handle it
         }
     } // End while loop
 
     // --- Update file offset ---
     // Lock needed? If file struct is shared, yes. Assume VFS guarantees file struct access is serialized or we need internal lock.
     // Let's assume caller (VFS) serializes access to 'file' struct, so no lock here.
     file->offset += total_bytes_read;
 
     return (int)total_bytes_read;
 }
 
 /**
  * @brief Writes data to an opened file.
  */
 int fat_write_internal(file_t *file, const void *buf, size_t len)
 {
      if (!file || !file->vnode || !file->vnode->data || !buf) {
         return -FS_ERR_INVALID_PARAM;
     }
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL);
     fat_fs_t *fs = fctx->fs;
 
     if (fctx->is_directory) {
         return -FS_ERR_IS_A_DIRECTORY;
     }
     if (len == 0) return 0;
 
     // Check write permissions from file flags
     if (!(file->flags & (O_WRONLY | O_RDWR))) {
         return -FS_ERR_PERMISSION_DENIED; // File not opened for writing
     }
 
     // --- Adjust offset for O_APPEND and handle initial state ---
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     off_t current_offset = file->offset; // Read current offset
 
     if (file->flags & O_APPEND) {
         current_offset = (off_t)fctx->file_size; // Seek to end
         file->offset = current_offset; // Update file struct offset
     }
     if (current_offset < 0) {
         current_offset = 0; // Correct negative offset
         file->offset = 0;
     }
 
     // Handle case: Empty file, need to allocate first cluster
     bool allocated_first = false;
     if (fctx->first_cluster < 2) {
         if (fctx->file_size != 0) {
              // Inconsistent state!
              spinlock_release_irqrestore(&fs->lock, irq_flags);
              terminal_printf("[FAT Write] Error: File size %u but first cluster invalid (%u).\n", fctx->file_size, fctx->first_cluster);
              return -FS_ERR_CORRUPT;
         }
         // Allocate the very first cluster for this file
         uint32_t new_cluster = fat_allocate_cluster(fs, 0); // Pass 0 as previous cluster
         if (new_cluster < 2) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             terminal_printf("[FAT Write] Error: Failed to allocate initial cluster (no space?).\n");
             return -FS_ERR_NO_SPACE;
         }
         fctx->first_cluster = new_cluster;
         fctx->dirty = true; // Mark context dirty (first cluster changed)
         allocated_first = true;
         terminal_printf("[FAT Write] Allocated initial cluster %u for file.\n", new_cluster);
     }
     uint32_t first_cluster = fctx->first_cluster; // Get potentially updated first cluster
     spinlock_release_irqrestore(&fs->lock, irq_flags); // --- Release lock before bulk I/O ---
 
     // --- Prepare for cluster traversal/allocation/write ---
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
     if (first_cluster < 2) return -FS_ERR_CORRUPT; // Should have been allocated or error above
 
 
     size_t total_bytes_written = 0;
     size_t bytes_left_to_write = len;
 
     // Calculate starting position
     uint32_t start_cluster_index = (uint32_t)(current_offset / cluster_size);
     uint32_t start_offset_in_cluster = (uint32_t)(current_offset % cluster_size);
     uint32_t current_cluster = first_cluster;
 
     // --- Traverse cluster chain to the starting cluster, allocating if needed ---
     for (uint32_t i = 0; i < start_cluster_index; i++) {
         uint32_t next_cluster;
         bool allocated_new = false;
 
         // Need lock to safely read/modify FAT table
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         if (get_next_res != FS_SUCCESS) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             terminal_printf("[FAT Write] Error: Failed to get next cluster after %u while seeking (err %d).\n", current_cluster, get_next_res);
             return -FS_ERR_IO; // Error reading FAT
         }
 
         if (next_cluster >= fs->eoc_marker) {
             // Reached end of chain, need to allocate a new cluster
             next_cluster = fat_allocate_cluster(fs, current_cluster); // Links current -> next
             if (next_cluster < 2) {
                  spinlock_release_irqrestore(&fs->lock, irq_flags);
                  terminal_printf("[FAT Write] Error: Failed to allocate cluster %u during seek/extend.\n", i + 1);
                  // Update file size based on partial write? Difficult. Return NO_SPACE.
                  return -FS_ERR_NO_SPACE;
             }
             fctx->dirty = true; // Mark context dirty (implicitly, FAT changed)
             allocated_new = true;
         }
         spinlock_release_irqrestore(&fs->lock, irq_flags); // Release lock after FAT operation
 
         current_cluster = next_cluster;
         if (allocated_new) {
              terminal_printf("[FAT Write] Allocated cluster %u during seek/extend.\n", current_cluster);
         }
     }
 
 
     // --- Write data cluster by cluster, allocating as needed ---
     while (bytes_left_to_write > 0)
     {
          if (current_cluster < 2 || current_cluster >= fs->eoc_marker) {
              // Should only happen if initial allocation failed or chain is corrupt
              terminal_printf("[FAT Write] Error: Invalid current cluster %u during write loop.\n", current_cluster);
              return -FS_ERR_CORRUPT; // Or FS_ERR_IO
          }
 
         size_t bytes_to_write_this_cluster = cluster_size - start_offset_in_cluster;
         if (bytes_to_write_this_cluster > bytes_left_to_write) {
             bytes_to_write_this_cluster = bytes_left_to_write;
         }
 
         // Write data to the current cluster via cached helper (no lock needed here)
         int wc = write_cluster_cached(fs, current_cluster, start_offset_in_cluster,
                                       (const uint8_t*)buf + total_bytes_written,
                                       bytes_to_write_this_cluster);
 
         if (wc < 0) {
              terminal_printf("[FAT Write] Error: Failed to write %u bytes to cluster %u (err %d).\n", bytes_to_write_this_cluster, current_cluster, wc);
              // Return error, partial write occurred. Update size?
              return wc;
         }
          if ((size_t)wc != bytes_to_write_this_cluster) {
              // Disk full during write? Or other buffer cache error?
              terminal_printf("[FAT Write] Warning: write_cluster_cached wrote %d bytes, expected %u.\n", wc, bytes_to_write_this_cluster);
              // Treat as partial write completed
              total_bytes_written += wc;
              break; // Stop writing
          }
 
         total_bytes_written += bytes_to_write_this_cluster;
         bytes_left_to_write -= bytes_to_write_this_cluster;
         start_offset_in_cluster = 0; // Subsequent writes start at offset 0
 
         // Get/Allocate the next cluster if more data needs writing
         if (bytes_left_to_write > 0) {
             uint32_t next_cluster;
             bool allocated_new = false;
             // Need lock to safely read/modify FAT table
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
              if (get_next_res != FS_SUCCESS) {
                  spinlock_release_irqrestore(&fs->lock, irq_flags);
                  terminal_printf("[FAT Write] Error: Failed to get next cluster after %u during write (err %d).\n", current_cluster, get_next_res);
                  break; // Partial write done, return bytes written so far
              }
 
              if (next_cluster >= fs->eoc_marker) {
                  // Allocate a new cluster
                  next_cluster = fat_allocate_cluster(fs, current_cluster);
                  if (next_cluster < 2) {
                      spinlock_release_irqrestore(&fs->lock, irq_flags);
                      terminal_printf("[FAT Write] Error: Failed to allocate next cluster (no space?).\n");
                      // Partial write done. Let the loop exit.
                      bytes_left_to_write = 0; // Force loop exit
                      break;
                  }
                  fctx->dirty = true; // Mark context dirty (implicitly, FAT changed)
                  allocated_new = true;
              }
             spinlock_release_irqrestore(&fs->lock, irq_flags); // Release lock after FAT operation
 
             current_cluster = next_cluster;
              if (allocated_new) {
                  terminal_printf("[FAT Write] Allocated next cluster %u.\n", current_cluster);
              }
         }
     } // End while loop
 
     // --- Update file offset and size ---
     // Lock required to update shared context state (file_size, dirty flag)
     irq_flags = spinlock_acquire_irqsave(&fs->lock);
     off_t final_offset = current_offset + total_bytes_written;
     file->offset = final_offset; // Update file struct offset
 
     // Update file size in context if we wrote past the old EOF
     if ((uint64_t)final_offset > fctx->file_size) {
         fctx->file_size = (uint32_t)final_offset; // Update size in context
         fctx->dirty = true; // Mark context dirty (size changed)
         // TODO: Update modification time/date here in context if tracking it
     }
     // Ensure dirty flag is set if we allocated first cluster even if size didn't change (e.g., write 0 bytes to new file)
     if (allocated_first) {
          fctx->dirty = true;
     }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     return (int)total_bytes_written; // Return total bytes successfully written
 }
 
 
 /**
  * @brief Changes the current read/write offset of an opened file.
  */
 off_t fat_lseek_internal(file_t *file, off_t offset, int whence)
 {
      if (!file || !file->vnode || !file->vnode->data) {
         return (off_t)-1; // Or set errno to EBADF
     }
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL);
     fat_fs_t *fs = fctx->fs;
 
     off_t new_offset;
 
     // Lock required to read file_size consistently
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     uint32_t file_size = fctx->file_size; // Read size while lock is held
     off_t current_offset = file->offset; // Read current offset
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // Calculate the new offset based on whence
     switch (whence) {
     case SEEK_SET:
         new_offset = offset;
         break;
     case SEEK_CUR:
         // Check for potential overflow before adding
         if ((offset > 0 && current_offset > (off_t)(LONG_MAX - offset)) ||
             (offset < 0 && current_offset < (off_t)(LONG_MIN - offset))) {
              // fs_set_errno(-FS_ERR_OVERFLOW);
              return (off_t)-1;
         }
         new_offset = current_offset + offset;
         break;
     case SEEK_END:
          // Check for potential overflow before adding
          if ((offset > 0 && (off_t)file_size > (off_t)(LONG_MAX - offset)) ||
              (offset < 0 && (off_t)file_size < (off_t)(LONG_MIN - offset))) {
               // fs_set_errno(-FS_ERR_OVERFLOW);
               return (off_t)-1;
          }
         new_offset = (off_t)file_size + offset;
         break;
     default:
         // fs_set_errno(-FS_ERR_INVALID_PARAM);
         return (off_t)-1; // Invalid whence
     }
 
     // Check if the resulting offset is negative, which is invalid
     if (new_offset < 0) {
          // fs_set_errno(-FS_ERR_INVALID_PARAM);
          return (off_t)-1;
     }
 
     // POSIX allows seeking beyond the end of the file.
     // A subsequent write will extend the file (potentially creating a hole).
     // A subsequent read will return 0 (EOF).
 
     // Update the file offset (assuming VFS serializes access to file struct)
     file->offset = new_offset;
 
     return new_offset; // Return the new offset
 }
 
 
 /**
  * @brief Closes an opened file.
  */
 int fat_close_internal(file_t *file)
 {
     if (!file || !file->vnode || !file->vnode->data) {
         return -FS_ERR_INVALID_PARAM;
     }
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL);
     fat_fs_t *fs = fctx->fs;
 
     int result = FS_SUCCESS;
 
     // Acquire lock to safely check/update dirty flag and directory entry
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     // If context is dirty, update the directory entry on disk
     if (fctx->dirty) {
         terminal_printf("[FAT Close] File context is dirty, updating directory entry (parent cluster %u, offset %u).\n",
                          fctx->dir_entry_cluster, fctx->dir_entry_offset);
 
         fat_dir_entry_t current_entry_data;
         bool read_ok = false;
 
         // Need to read the existing entry first to preserve other fields (timestamps, attributes etc.)
         // We use read_directory_sector which gets the whole sector, then extract the entry.
         size_t sec_size = fs->bytes_per_sector;
         if (sec_size > 0) {
             uint32_t sector_offset_in_chain = fctx->dir_entry_offset / sec_size;
             size_t offset_in_sector = fctx->dir_entry_offset % sec_size;
 
              // Need temporary buffer to read sector
              uint8_t* sector_buffer = kmalloc(sec_size);
              if (sector_buffer) {
                  // read_directory_sector needs lock held? Yes, if FAT might be modified concurrently.
                  // It calls fat_get_next_cluster internally. Keep lock held.
                  if (read_directory_sector(fs, fctx->dir_entry_cluster,
                                             sector_offset_in_chain,
                                             sector_buffer) == FS_SUCCESS)
                  {
                      memcpy(&current_entry_data, sector_buffer + offset_in_sector, sizeof(fat_dir_entry_t));
                      read_ok = true;
                  } else {
                       terminal_printf("[FAT Close] Error: Failed to read directory sector for update.\n");
                       result = -FS_ERR_IO;
                  }
                  kfree(sector_buffer);
              } else {
                   terminal_printf("[FAT Close] Error: Failed to allocate buffer for directory read.\n");
                   result = -FS_ERR_OUT_OF_MEMORY;
              }
         } else {
              terminal_printf("[FAT Close] Error: Invalid sector size 0.\n");
              result = -FS_ERR_INVALID_FORMAT;
         }
 
 
         // If we successfully read the old entry, update it and write back
         if (read_ok) {
             // Update the fields that might have changed
             current_entry_data.file_size = fctx->file_size;
             current_entry_data.first_cluster_low  = (uint16_t)(fctx->first_cluster & 0xFFFF);
             current_entry_data.first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
             // TODO: Update modification time/date in current_entry_data
 
             // update_directory_entry writes the modified entry back using buffer cache
             int update_res = update_directory_entry(fs, fctx->dir_entry_cluster,
                                                     fctx->dir_entry_offset, &current_entry_data);
             if (update_res != FS_SUCCESS) {
                  terminal_printf("[FAT Close] Error: Failed to update directory entry (err %d).\n", update_res);
                  if (result == FS_SUCCESS) result = update_res; // Record error if not already set
             } else {
                  // Successfully updated entry, consider flushing FAT + Dir changes
                  // flush_fat_table(fs); // Flush FAT if needed (set dirty by alloc/free)
                  // buffer_cache_sync_device(...); // Sync directory block
                  terminal_printf("[FAT Close] Directory entry updated.\n");
             }
         }
         // We clear the dirty flag even if update failed, as we attempted it.
         fctx->dirty = false;
     }
 
     // Release lock before freeing memory
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // --- Free allocated resources ---
     // Free the FAT file context
     kfree(file->vnode->data);
     file->vnode->data = NULL; // Prevent dangling pointer
 
     // Free the VFS vnode structure
     kfree(file->vnode);
     file->vnode = NULL; // Prevent dangling pointer in file_t
 
     // Note: The VFS layer is responsible for freeing the file_t structure itself.
 
     return result; // Return status (primarily from directory entry update attempt)
 }