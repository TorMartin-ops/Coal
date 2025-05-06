/**
 * @file fat_io.c
 * @brief File I/O operations implementation for FAT filesystem driver.
 * @version 1.6 - Fixed build errors, removed incorrect FAT flush on close.
 *
 * Production Level Revision:
 * - Implemented VFS read and write operations with detailed logging.
 * - Handles cluster chain traversal, allocation on write, EOF, errors.
 * - Uses buffer cache for all cluster/sector I/O.
 * - Includes locking for shared filesystem structure access (FAT table, context).
 * - Updates file size and context dirty flag on write.
 * - Logging now uses basic serial functions only.
 * - Allocates the first cluster if writing to an empty file (first_cluster == 0).
 * - **Correction:** Removed inappropriate call to flush_fat_table from close.
 * - **Correction:** Fixed incorrect usage of serial_write for formatted logging.
 * - TODO: Timestamp updates on write are currently disabled/placeholders.
 */

// --- Core Includes ---
#include "fat_io.h"
#include "fat_core.h"       // Defines fat_fs_t, fat_file_context_t, FAT_TYPE_*, FAT_ATTR_*, FAT_ATTR_LONG_NAME, fat_dir_entry_t etc.
#include "fat_utils.h"      // fat_cluster_to_lba, fat_get_current_timestamp (placeholder)
#include "fat_alloc.h"      // fat_get_next_cluster, fat_allocate_cluster
#include "fat_dir.h"        // update_directory_entry (needed for close/flush), read_directory_sector (used in close)
#include "buffer_cache.h"   // buffer_get, buffer_release, buffer_mark_dirty
#include "spinlock.h"       // spinlock_t, spinlock_acquire_irqsave, spinlock_release_irqrestore
#include "serial.h"         // serial_write, serial_print_hex
#include "sys_file.h"       // O_* flags, SEEK_* defines
#include "kmalloc.h"        // Kernel memory allocation (kfree - needed by helpers)
#include "fs_errno.h"       // Filesystem error codes
#include <string.h>         // memcpy, memset
#include "assert.h"         // KERNEL_ASSERT
#include <libc/limits.h>    // LONG_MAX, LONG_MIN, SIZE_MAX
#include <libc/stddef.h>    // NULL, size_t
#include <libc/stdbool.h>   // bool
#include <libc/stdarg.h>    // varargs for printf (though not used in macros now)
#include "time.h"           // kernel_get_time(), kernel_time_t (placeholder)
#include "terminal.h"       // terminal_printf FOR FORMATTED LOGGING

/* --- Helper Macros --- */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* --- Cluster I/O Helpers (Implementations from previous context) --- */

/**
 * @brief Reads a block of data from a specific cluster or FAT12/16 root dir area.
 */
int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_location, void *buf, size_t len)
{
    KERNEL_ASSERT(fs != NULL && buf != NULL, "NULL fs or buf");
    KERNEL_ASSERT(len > 0, "Zero length read");

    uint32_t sector_size = fs->bytes_per_sector;
    uint32_t location_size;
    uint32_t start_lba;

    if (cluster == 0 && (fs->type == FAT_TYPE_FAT12 || fs->type == FAT_TYPE_FAT16)) {
        location_size = fs->root_dir_sectors * fs->bytes_per_sector;
        start_lba = fs->root_dir_start_lba;
        KERNEL_ASSERT(offset_in_location < location_size, "Offset out of root dir bounds");
        KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of root dir bounds");
        serial_write("[FAT IO TRACE] read_cluster_cached: Reading from FAT12/16 Root Dir\n");
        serial_write("  Start LBA=0x"); serial_print_hex(start_lba);
        serial_write("  Offset=0x"); serial_print_hex(offset_in_location);
        serial_write("  Len=0x"); serial_print_hex((uint32_t)len); serial_write("\n");
    } else if (cluster >= 2) {
        location_size = fs->cluster_size_bytes;
        start_lba = fat_cluster_to_lba(fs, cluster);
        if (start_lba == 0) {
            serial_write("[FAT IO ERROR] read_cluster_cached: Failed to convert data cluster to LBA\n");
            serial_write("  Cluster=0x"); serial_print_hex(cluster); serial_write("\n");
            return FS_ERR_IO;
        }
        KERNEL_ASSERT(offset_in_location < location_size, "Offset out of cluster bounds");
        KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of cluster bounds");
        serial_write("[FAT IO TRACE] read_cluster_cached: Reading from Cluster\n");
        serial_write("  Cluster=0x"); serial_print_hex(cluster);
        serial_write("  Start LBA=0x"); serial_print_hex(start_lba);
        serial_write("  Offset=0x"); serial_print_hex(offset_in_location);
        serial_write("  Len=0x"); serial_print_hex((uint32_t)len); serial_write("\n");
    } else {
        serial_write("[FAT IO ERROR] read_cluster_cached: Invalid cluster number for read\n");
        serial_write("  Cluster=0x"); serial_print_hex(cluster); serial_write("\n");
        return FS_ERR_INVALID_PARAM;
    }

    KERNEL_ASSERT(sector_size > 0, "Invalid sector size");
    KERNEL_ASSERT(location_size > 0, "Invalid location size");

    uint32_t start_sector_in_location = offset_in_location / sector_size;
    uint32_t end_sector_in_location   = (offset_in_location + len - 1) / sector_size;

    size_t bytes_read_total = 0;
    uint8_t *dest_ptr = (uint8_t *)buf;

    for (uint32_t sec_idx = start_sector_in_location; sec_idx <= end_sector_in_location; sec_idx++) {
        uint32_t current_lba = start_lba + sec_idx;
        serial_write("[FAT IO TRACE] read_cluster_cached:   Reading LBA\n");
        serial_write("    LBA=0x"); serial_print_hex(current_lba);
        serial_write("    SecInLoc=0x"); serial_print_hex(sec_idx); serial_write("\n");

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
        if (!b) {
            serial_write("[FAT IO ERROR] read_cluster_cached:   Failed to get buffer for LBA\n");
            serial_write("    LBA=0x"); serial_print_hex(current_lba); serial_write("\n");
            return FS_ERR_IO;
        }

        size_t offset_within_this_sector = (sec_idx == start_sector_in_location) ? (offset_in_location % sector_size) : 0;
        size_t bytes_to_copy_from_this_sector = sector_size - offset_within_this_sector;
        size_t bytes_remaining_to_read = len - bytes_read_total;
        if (bytes_to_copy_from_this_sector > bytes_remaining_to_read) { bytes_to_copy_from_this_sector = bytes_remaining_to_read; }

        serial_write("[FAT IO TRACE] read_cluster_cached:     Copying bytes\n");
        serial_write("      Count=0x"); serial_print_hex((uint32_t)bytes_to_copy_from_this_sector);
        serial_write("      SecOff=0x"); serial_print_hex((uint32_t)offset_within_this_sector);
        serial_write("      BufOff=0x"); serial_print_hex((uint32_t)bytes_read_total); serial_write("\n");

        memcpy(dest_ptr, b->data + offset_within_this_sector, bytes_to_copy_from_this_sector);
        buffer_release(b);

        dest_ptr += bytes_to_copy_from_this_sector;
        bytes_read_total += bytes_to_copy_from_this_sector;
    }

    KERNEL_ASSERT(bytes_read_total == len, "Bytes read mismatch");
    serial_write("[FAT IO TRACE] read_cluster_cached: Read cluster successful\n");
    serial_write("  Cluster=0x"); serial_print_hex(cluster);
    serial_write("  BytesRead=0x"); serial_print_hex((uint32_t)bytes_read_total); serial_write("\n");
    return (int)bytes_read_total;
}

/**
 * @brief Writes a block of data to a specific data cluster using the buffer cache.
 */
int write_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, const void *buf, size_t len)
{
    KERNEL_ASSERT(fs != NULL && buf != NULL, "NULL fs or buf");
    KERNEL_ASSERT(cluster >= 2, "Invalid cluster number for write (must be >= 2)");
    KERNEL_ASSERT(offset_in_cluster < fs->cluster_size_bytes, "Offset out of bounds");
    KERNEL_ASSERT(len > 0 && offset_in_cluster + len <= fs->cluster_size_bytes, "Length out of bounds");

    uint32_t sector_size = fs->bytes_per_sector;
    KERNEL_ASSERT(sector_size > 0, "Invalid sector size");

    uint32_t start_sector_in_cluster = offset_in_cluster / sector_size;
    uint32_t end_sector_in_cluster   = (offset_in_cluster + len - 1) / sector_size;

    uint32_t cluster_lba = fat_cluster_to_lba(fs, cluster);
    if (cluster_lba == 0) {
        serial_write("[FAT IO ERROR] write_cluster_cached: Failed to convert cluster to LBA\n");
        serial_write("  Cluster=0x"); serial_print_hex(cluster); serial_write("\n");
        return FS_ERR_IO;
    }

    serial_write("[FAT IO TRACE] write_cluster_cached: Writing to Cluster\n");
    serial_write("  Cluster=0x"); serial_print_hex(cluster);
    serial_write("  Start LBA=0x"); serial_print_hex(cluster_lba);
    serial_write("  Offset=0x"); serial_print_hex(offset_in_cluster);
    serial_write("  Len=0x"); serial_print_hex((uint32_t)len); serial_write("\n");

    size_t bytes_written_total = 0;
    const uint8_t *src_ptr = (const uint8_t *)buf;
    int result = FS_SUCCESS;

    for (uint32_t sec_idx = start_sector_in_cluster; sec_idx <= end_sector_in_cluster; sec_idx++) {
         uint32_t current_lba = cluster_lba + sec_idx;
         serial_write("[FAT IO TRACE] write_cluster_cached:   Writing LBA\n");
         serial_write("    LBA=0x"); serial_print_hex(current_lba);
         serial_write("    SecInClus=0x"); serial_print_hex(sec_idx); serial_write("\n");

         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
         if (!b) {
             serial_write("[FAT IO ERROR] write_cluster_cached:   Failed to get buffer for LBA\n");
             serial_write("    LBA=0x"); serial_print_hex(current_lba); serial_write("\n");
             result = FS_ERR_IO; goto write_cluster_cleanup;
         }

        size_t offset_within_this_sector = (sec_idx == start_sector_in_cluster) ? (offset_in_cluster % sector_size) : 0;
        size_t bytes_to_copy_to_this_sector = sector_size - offset_within_this_sector;
        size_t bytes_remaining_to_write = len - bytes_written_total;
        if (bytes_to_copy_to_this_sector > bytes_remaining_to_write) { bytes_to_copy_to_this_sector = bytes_remaining_to_write; }

        serial_write("[FAT IO TRACE] write_cluster_cached:     Copying bytes\n");
        serial_write("      Count=0x"); serial_print_hex((uint32_t)bytes_to_copy_to_this_sector);
        serial_write("      BufOff=0x"); serial_print_hex((uint32_t)bytes_written_total);
        serial_write("      SecOff=0x"); serial_print_hex((uint32_t)offset_within_this_sector); serial_write("\n");

        memcpy(b->data + offset_within_this_sector, src_ptr, bytes_to_copy_to_this_sector);
        buffer_mark_dirty(b);
        buffer_release(b);

        src_ptr += bytes_to_copy_to_this_sector;
        bytes_written_total += bytes_to_copy_to_this_sector;
    }

write_cluster_cleanup:
    if (result != FS_SUCCESS) {
        serial_write("[FAT IO ERROR] write_cluster_cached: Error occurred during cached write\n");
        serial_write("  ErrorCode=0x"); serial_print_hex((uint32_t)result);
        serial_write("  BytesWritten=0x"); serial_print_hex((uint32_t)bytes_written_total); serial_write("\n");
        return result;
    }

    KERNEL_ASSERT(bytes_written_total == len, "Bytes written mismatch");
    serial_write("[FAT IO TRACE] write_cluster_cached: Write cluster successful\n");
    serial_write("  Cluster=0x"); serial_print_hex(cluster);
    serial_write("  BytesWritten=0x"); serial_print_hex((uint32_t)bytes_written_total); serial_write("\n");
    return (int)bytes_written_total;
}


/* --- VFS Operation Implementations --- */

/**
 * @brief Reads data from an opened file. Implements VFS read operation.
 */
 int fat_read_internal(file_t *file, void *buf, size_t len)
 {
     // --- 1. Input Validation ---
     if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
         serial_write("[FAT IO ERROR] fat_read_internal: Invalid parameters\n");
         return FS_ERR_INVALID_PARAM;
     }
     if (len == 0) return 0;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     if (fctx->is_directory) {
         serial_write("[FAT IO ERROR] fat_read_internal: Attempt to read from a directory\n");
         return FS_ERR_IS_A_DIRECTORY;
     }
 
     uintptr_t irq_flags;
     int result = FS_SUCCESS;
     size_t total_bytes_read = 0;
 
     // --- 2. Determine Read Bounds ---
     irq_flags = spinlock_acquire_irqsave(&fs->lock); // Use FS lock for reading context
     off_t current_offset = file->offset;
     uint32_t file_size = fctx->file_size;
     uint32_t first_cluster = fctx->first_cluster;
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     serial_write("[FAT IO TRACE] fat_read_internal: Enter\n");
     serial_write("  Offset=0x"); serial_print_hex((uint32_t)current_offset);
     serial_write("  ReqLen=0x"); serial_print_hex((uint32_t)len);
     serial_write("  FileSize=0x"); serial_print_hex(file_size);
     serial_write("  FirstClu=0x"); serial_print_hex(first_cluster); serial_write("\n");
 
     if (current_offset < 0) {
         serial_write("[FAT IO ERROR] fat_read_internal: Negative file offset\n");
         return FS_ERR_INVALID_PARAM;
     }
     if ((uint64_t)current_offset >= file_size) {
         serial_write("[FAT IO TRACE] fat_read_internal: Read attempt at or beyond EOF\n");
         return 0;
     }
 
     uint64_t remaining_in_file = (uint64_t)file_size - current_offset;
     size_t max_readable = MIN(len, (size_t)remaining_in_file);
     if (max_readable == 0) { return 0; }
     len = max_readable;
     serial_write("[FAT IO TRACE] fat_read_internal: Adjusted read length\n");
     serial_write("  MaxRead=0x"); serial_print_hex((uint32_t)len); serial_write("\n");
 
     // --- 3. Prepare for Cluster Traversal ---
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) { serial_write("[FAT IO ERROR] fat_read_internal: Invalid cluster size 0\n"); return FS_ERR_INVALID_FORMAT; }
     if (first_cluster < 2 && file_size > 0) {
         serial_write("[FAT IO ERROR] fat_read_internal: File size > 0 but first cluster invalid\n");
         return FS_ERR_CORRUPT;
     }
     if (first_cluster < 2) { return 0; } // file_size must be 0 here
 
     uint32_t current_cluster = first_cluster;
     uint32_t cluster_index = (uint32_t)(current_offset / cluster_size);
     uint32_t offset_in_current_cluster = (uint32_t)(current_offset % cluster_size);
 
     // --- 4. Traverse Cluster Chain to Starting Cluster ---
     serial_write("[FAT IO TRACE] fat_read_internal: Seeking to cluster index\n");
     serial_write("  TargetIdx=0x"); serial_print_hex(cluster_index); serial_write("\n");
     for (uint32_t i = 0; i < cluster_index; i++) {
         uint32_t next_cluster;
         serial_write("[FAT IO TRACE] fat_read_internal:   Seeking\n");
         irq_flags = spinlock_acquire_irqsave(&fs->lock);
         result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         spinlock_release_irqrestore(&fs->lock, irq_flags);
 
         if (result != FS_SUCCESS) {
             serial_write("[FAT IO ERROR] fat_read_internal:   Seek failed: Error getting next cluster\n");
             return FS_ERR_IO;
         }
         if (next_cluster >= fs->eoc_marker) {
             serial_write("[FAT IO ERROR] fat_read_internal:   Seek failed: Corrupt file - EOC found early\n");
             return FS_ERR_CORRUPT;
         }
         current_cluster = next_cluster;
     }
     serial_write("[FAT IO TRACE] fat_read_internal: Seek successful\n");
     serial_write("  StartClu=0x"); serial_print_hex(current_cluster);
     serial_write("  OffsetInClu=0x"); serial_print_hex(offset_in_current_cluster); serial_write("\n");
 
     // --- 5. Read Data Cluster by Cluster ---
     while (total_bytes_read < len) {
         if (current_cluster < 2 || current_cluster >= fs->eoc_marker) {
             serial_write("[FAT IO ERROR] fat_read_internal: Corrupt file: Invalid cluster in read loop\n");
             result = FS_ERR_CORRUPT; goto cleanup_read;
         }
 
         size_t bytes_to_read_this_cluster = MIN(cluster_size - offset_in_current_cluster, len - total_bytes_read);
         serial_write("[FAT IO TRACE] fat_read_internal: Loop: Reading from cluster\n");
         serial_write("  Cluster=0x"); serial_print_hex(current_cluster);
         serial_write("  Offset=0x"); serial_print_hex(offset_in_current_cluster);
         serial_write("  Bytes=0x"); serial_print_hex((uint32_t)bytes_to_read_this_cluster); serial_write("\n");
 
         result = read_cluster_cached(fs, current_cluster, offset_in_current_cluster, (uint8_t*)buf + total_bytes_read, bytes_to_read_this_cluster);
 
         if (result < 0) {
             serial_write("[FAT IO ERROR] fat_read_internal:   read_cluster_cached failed\n");
             goto cleanup_read;
         }
         if ((size_t)result != bytes_to_read_this_cluster) {
             serial_write("[FAT IO ERROR] fat_read_internal:   Short read from read_cluster_cached\n");
             result = FS_ERR_IO; goto cleanup_read;
         }
 
         total_bytes_read += bytes_to_read_this_cluster;
         offset_in_current_cluster = 0; // Subsequent reads from this cluster start at offset 0
 
         if (total_bytes_read < len) {
             uint32_t next_cluster;
             serial_write("[FAT IO TRACE] fat_read_internal:   Getting next cluster after\n");
             irq_flags = spinlock_acquire_irqsave(&fs->lock);
             result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
             spinlock_release_irqrestore(&fs->lock, irq_flags);
 
             if (result != FS_SUCCESS) {
                 serial_write("[FAT IO ERROR] fat_read_internal:   Failed to get next cluster\n");
                 result = FS_ERR_IO; goto cleanup_read;
             }
 
             serial_write("[FAT IO TRACE] fat_read_internal:   Moved to next cluster\n");
             serial_write("    NextClu=0x"); serial_print_hex(next_cluster); serial_write("\n");
             current_cluster = next_cluster;
             if (current_cluster >= fs->eoc_marker) {
                 serial_write("[FAT IO WARN] fat_read_internal:   EOF reached mid-read\n");
                 break; // Reached end of allocated clusters
             }
         }
     } // End while loop
 
     result = FS_SUCCESS; // Mark success if loop completed naturally or EOF hit mid-read
 
 cleanup_read:
     if (total_bytes_read > 0) {
         serial_write("[FAT IO TRACE] fat_read_internal: Successfully read total bytes\n");
         serial_write("  TotalRead=0x"); serial_print_hex((uint32_t)total_bytes_read); serial_write("\n");
     }
     serial_write("[FAT IO TRACE] fat_read_internal: Exit\n");
     serial_write("  ReturnValue=0x"); serial_print_hex((uint32_t)((result < 0) ? result : (int)total_bytes_read)); serial_write("\n");
     return (result < 0) ? result : (int)total_bytes_read;
 }
 
 
 
 /**
  * @brief Closes an opened file. Implements VFS close.
  */
 int fat_close_internal(file_t *file)
 {
     if (!file || !file->vnode || !file->vnode->data) { return FS_ERR_BAD_F; }
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     serial_write("[FAT IO TRACE] fat_close_internal: Enter: Closing file context\n");
     serial_write("  fctx=0x"); serial_print_hex((uintptr_t)fctx);
     serial_write("  dirty=0x"); serial_print_hex((uint32_t)fctx->dirty); serial_write("\n");
 
     int update_result = FS_SUCCESS;
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     if (fctx->dirty) {
         serial_write("[FAT IO DEBUG] fat_close_internal: Context dirty, updating directory entry\n");
         serial_write("  DirClu=0x"); serial_print_hex(fctx->dir_entry_cluster);
         serial_write("  DirOff=0x"); serial_print_hex(fctx->dir_entry_offset); serial_write("\n");
 
         fat_dir_entry_t existing_entry;
         uint8_t *sector_buf = kmalloc(fs->bytes_per_sector);
         if (!sector_buf) {
              serial_write("[FAT IO ERROR] fat_close_internal: Failed to allocate sector buffer!\n");
              update_result = FS_ERR_OUT_OF_MEMORY;
         } else {
             serial_write("[FAT IO TRACE] fat_close_internal:   Reading existing dir entry sector\n");
             int read_sec_res = read_directory_sector(fs, fctx->dir_entry_cluster, fctx->dir_entry_offset / fs->bytes_per_sector, sector_buf);
             if (read_sec_res == FS_SUCCESS) {
                 memcpy(&existing_entry, sector_buf + (fctx->dir_entry_offset % fs->bytes_per_sector), sizeof(fat_dir_entry_t));
                 serial_write("[FAT IO TRACE] fat_close_internal:   Updating entry fields\n");
                 existing_entry.file_size = fctx->file_size;
                 existing_entry.first_cluster_low = (uint16_t)(fctx->first_cluster & 0xFFFF);
                 existing_entry.first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
                 // TODO: Update timestamps if tracked in context
                 // uint16_t current_time, current_date;
                 // fat_get_current_timestamp(&current_time, &current_date);
                 // existing_entry.write_time = current_time;
                 // existing_entry.write_date = current_date;
                 // existing_entry.last_access_date = current_date;
 
                 serial_write("[FAT IO TRACE] fat_close_internal:   Writing updated dir entry\n");
                 update_result = update_directory_entry(fs, fctx->dir_entry_cluster, fctx->dir_entry_offset, &existing_entry);
                 if (update_result != FS_SUCCESS) {
                     serial_write("[FAT IO ERROR] fat_close_internal:   Failed to update directory entry\n");
                 } else {
                     serial_write("[FAT IO DEBUG] fat_close_internal:   Directory entry update successful\n");
                     fctx->dirty = false; // Clear dirty flag only on successful write back
                 }
             } else {
                  serial_write("[FAT IO ERROR] fat_close_internal:   Failed to read directory sector for update\n");
                  update_result = read_sec_res; // Propagate read error
             }
             kfree(sector_buf); // Free the temp buffer
         }
     } // End if (fctx->dirty)
 
     spinlock_release_irqrestore(&fs->lock, irq_flags); // Release FS lock
 
     kfree(fctx); // Free the file context memory
     file->vnode->data = NULL; // Clear pointer in vnode (vnode itself freed by VFS)
 
     serial_write("[FAT IO TRACE] fat_close_internal: Exit\n");
     serial_write("  ReturnValue=0x"); serial_print_hex((uint32_t)update_result); serial_write("\n");
     return update_result; // Return status from directory entry update
 }


/**
 * @brief Writes data to an opened file. Implements VFS write operation.
 * Handles cluster allocation, EOF extension, and updating file metadata.
 */
 int fat_write_internal(file_t *file, const void *buf, size_t len)
 {
     // --- 1. Input Validation ---
     if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
         serial_write("[FAT IO ERROR] fat_write_internal: Invalid parameters\n");
         // Log details if possible...
         return FS_ERR_INVALID_PARAM;
     }
     if (len == 0) return 0;
 
     fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
     KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
     fat_fs_t *fs = fctx->fs;
 
     if (fctx->is_directory) {
         serial_write("[FAT IO ERROR] fat_write_internal: Cannot write to a directory\n");
         return FS_ERR_IS_A_DIRECTORY;
     }
     if (!(file->flags & (O_WRONLY | O_RDWR))) {
         serial_write("[FAT IO ERROR] fat_write_internal: File not opened for writing\n");
         return FS_ERR_PERMISSION_DENIED;
     }
 
     uintptr_t irq_flags;
     int result = FS_SUCCESS;
     size_t total_bytes_written = 0;
     bool file_metadata_changed = false;
 
     // --- 2. Determine Write Position & Handle O_APPEND ---
     irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock for reading context safely
     off_t current_offset = file->offset;
     uint32_t file_size_before_write = fctx->file_size;
     if (file->flags & O_APPEND) { current_offset = (off_t)file_size_before_write; }
     if (current_offset < 0) {
         spinlock_release_irqrestore(&fs->lock, irq_flags);
         serial_write("[FAT IO ERROR] fat_write_internal: Negative file offset\n");
         return FS_ERR_INVALID_PARAM;
     }
     uint32_t first_cluster = fctx->first_cluster;
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     serial_write("[FAT IO TRACE] fat_write_internal: Enter\n");
     serial_write("  Offset=0x"); serial_print_hex((uint32_t)current_offset);
     serial_write("  ReqLen=0x"); serial_print_hex((uint32_t)len);
     serial_write("  FileSize=0x"); serial_print_hex(file_size_before_write);
     serial_write("  FirstClu=0x"); serial_print_hex(first_cluster); serial_write("\n");
 
     // --- 3. Prepare for Cluster Traversal/Allocation ---
     size_t cluster_size = fs->cluster_size_bytes;
     if (cluster_size == 0) {
         serial_write("[FAT IO ERROR] fat_write_internal: Invalid cluster size 0\n");
         return FS_ERR_INVALID_FORMAT;
     }
 
     // --- 4. Allocate Initial Cluster if Necessary ---
     if (first_cluster < 2) {
         if (current_offset != 0) {
              serial_write("[FAT IO ERROR] fat_write_internal: Attempt to write at offset in empty, unallocated file\n");
              return FS_ERR_INVALID_PARAM;
         }
 
         serial_write("[FAT IO TRACE] fat_write_internal: Allocating initial cluster for write.\n");
         irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock before allocation
         uint32_t new_cluster = fat_allocate_cluster(fs, 0); // Allocate first cluster
         if (new_cluster < 2) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             serial_write("[FAT IO ERROR] fat_write_internal: Failed to allocate initial cluster (no space?) - returning FS_ERR_NO_SPACE\n");
             return FS_ERR_NO_SPACE;
         }
         serial_write("[FAT IO DEBUG] fat_write_internal: Allocated initial cluster\n");
         serial_write("  NewCluster=0x"); serial_print_hex(new_cluster); serial_write("\n");
 
         fctx->first_cluster = new_cluster; // Set the cluster in the context
 
         // ********** FIX 1: Update Dir Entry Cluster **********
         serial_write("[FAT IO DEBUG] fat_write_internal: Immediately updating directory entry cluster...\n");
         int rc = update_directory_entry_first_cluster_now(fs, fctx); // Assumes lock is held
         if (rc != FS_SUCCESS) {
             serial_write("[FAT IO ERROR] fat_write_internal: Failed to update dir entry cluster! Rolling back...\n");
             fat_free_cluster_chain(fs, new_cluster); // Free the just allocated cluster
             fctx->first_cluster = 0; // Reset context state
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             return rc; // Return the error from the update function
         }
         serial_write("[FAT IO DEBUG] fat_write_internal: Directory entry cluster updated.\n");
         // ********** END OF FIX 1 **********
 
         first_cluster = new_cluster; // Update local variable for subsequent logic
         file_metadata_changed = true; // Mark metadata changed
         fctx->dirty = true;           // Mark context dirty
 
         spinlock_release_irqrestore(&fs->lock, irq_flags); // Release lock after successful alloc+update
     }
     KERNEL_ASSERT(first_cluster >= 2, "First cluster invalid after initial check/alloc");
 
     // --- 5. Traverse/Extend Cluster Chain to Starting Cluster ---
     uint32_t current_cluster = first_cluster;
     uint32_t cluster_index = (uint32_t)(current_offset / cluster_size);
     uint32_t offset_in_current_cluster = (uint32_t)(current_offset % cluster_size);
 
     serial_write("[FAT IO TRACE] fat_write_internal: Seeking/extending to cluster index\n");
     serial_write("  TargetIdx=0x"); serial_print_hex(cluster_index); serial_write("\n");
     for (uint32_t i = 0; i < cluster_index; i++) {
         uint32_t next_cluster;
         bool allocated_new = false;
         serial_write("[FAT IO TRACE] fat_write_internal:   Seeking/Extending\n");
         serial_write("    CurrentClu=0x"); serial_print_hex(current_cluster);
         serial_write("    Idx=0x"); serial_print_hex(i); serial_write("\n");
 
         irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock for FAT access
         int find_result = fat_get_next_cluster(fs, current_cluster, &next_cluster);
         if (find_result != FS_SUCCESS) {
             spinlock_release_irqrestore(&fs->lock, irq_flags);
             serial_write("[FAT IO ERROR] fat_write_internal:   Seek failed: Error getting next cluster\n");
             result = FS_ERR_IO; goto cleanup_write;
         }
 
         if (next_cluster >= fs->eoc_marker) {
             serial_write("[FAT IO TRACE] fat_write_internal:   Allocating new cluster after\n");
             next_cluster = fat_allocate_cluster(fs, current_cluster); // Allocates AND links
             if (next_cluster < 2) {
                 spinlock_release_irqrestore(&fs->lock, irq_flags);
                 serial_write("[FAT IO ERROR] fat_write_internal:   Seek failed: Failed to allocate cluster - returning FS_ERR_NO_SPACE\n");
                 result = FS_ERR_NO_SPACE; goto cleanup_write;
             }
             serial_write("[FAT IO DEBUG] fat_write_internal:   Allocated cluster during seek/extend\n");
             fctx->dirty = true; // Mark context dirty because chain changed
             file_metadata_changed = true;
             allocated_new = true;
         }
         spinlock_release_irqrestore(&fs->lock, irq_flags); // Release lock
 
         current_cluster = next_cluster;
         if (!allocated_new) {
             serial_write("[FAT IO TRACE] fat_write_internal:   Moved to existing next cluster\n");
             serial_write("    NextClu=0x"); serial_print_hex(current_cluster); serial_write("\n");
         }
     }
     serial_write("[FAT IO TRACE] fat_write_internal: Seek/extend successful\n");
     serial_write("  StartClu=0x"); serial_print_hex(current_cluster);
     serial_write("  OffsetInClu=0x"); serial_print_hex(offset_in_current_cluster); serial_write("\n");
 
     // --- 6. Write Data Cluster by Cluster, Allocating as Needed ---
     while (total_bytes_written < len) {
          if (current_cluster < 2 || current_cluster >= fs->eoc_marker) {
             serial_write("[FAT IO ERROR] fat_write_internal: Corrupt state: Invalid cluster in write loop\n");
             result = FS_ERR_CORRUPT; goto cleanup_write;
          }
 
         size_t bytes_to_write_this_cluster = MIN(cluster_size - offset_in_current_cluster, len - total_bytes_written);
         serial_write("[FAT IO TRACE] fat_write_internal: Loop: Writing to cluster\n");
         serial_write("  Cluster=0x"); serial_print_hex(current_cluster);
         serial_write("  Offset=0x"); serial_print_hex(offset_in_current_cluster);
         serial_write("  Bytes=0x"); serial_print_hex((uint32_t)bytes_to_write_this_cluster); serial_write("\n");
 
         int write_res = write_cluster_cached(fs, current_cluster, offset_in_current_cluster, (const uint8_t*)buf + total_bytes_written, bytes_to_write_this_cluster);
         if (write_res < 0) {
             serial_write("[FAT IO ERROR] fat_write_internal:   write_cluster_cached failed\n");
             result = write_res; goto cleanup_write;
         }
         if ((size_t)write_res != bytes_to_write_this_cluster) {
              serial_write("[FAT IO ERROR] fat_write_internal:   Short write from write_cluster_cached\n");
              result = FS_ERR_IO; goto cleanup_write;
         }
 
         total_bytes_written += bytes_to_write_this_cluster;
         offset_in_current_cluster = 0; // Reset offset for next iteration within this cluster
 
         if (total_bytes_written < len) {
             uint32_t next_cluster;
             bool allocated_new = false;
             int alloc_res = FS_SUCCESS;
             serial_write("[FAT IO TRACE] fat_write_internal:   Getting/Allocating next cluster after\n");
             serial_write("    CurrentClu=0x"); serial_print_hex(current_cluster); serial_write("\n");
 
             irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock for FAT access
             int find_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
             if (find_res == FS_SUCCESS && next_cluster >= fs->eoc_marker) {
                 serial_write("[FAT IO TRACE] fat_write_internal:   Allocating next cluster after EOC\n");
                 next_cluster = fat_allocate_cluster(fs, current_cluster); // Allocates AND links
                 if (next_cluster < 2) {
                     alloc_res = FS_ERR_NO_SPACE;
                     serial_write("[FAT IO ERROR] fat_write_internal:   Failed to allocate next cluster (no space?)\n");
                 } else {
                     serial_write("[FAT IO DEBUG] fat_write_internal:   Allocated next cluster\n");
                     fctx->dirty = true; // Mark context dirty because chain changed
                     file_metadata_changed = true;
                     allocated_new = true;
                 }
             } else if (find_res != FS_SUCCESS) {
                 alloc_res = FS_ERR_IO;
                 serial_write("[FAT IO ERROR] fat_write_internal:   Failed to get next cluster\n");
             }
             spinlock_release_irqrestore(&fs->lock, irq_flags); // Release lock
 
             if (alloc_res != FS_SUCCESS) {
                 result = alloc_res;
                 serial_write("[FAT IO ERROR] fat_write_internal:   Aborting write due to allocation/find error\n");
                 goto cleanup_write;
             }
             current_cluster = next_cluster;
             if (!allocated_new) {
                  serial_write("[FAT IO TRACE] fat_write_internal:   Moving to existing next cluster\n");
                  serial_write("    NextClu=0x"); serial_print_hex(current_cluster); serial_write("\n");
             }
         }
     } // End while loop
 
     KERNEL_ASSERT(total_bytes_written == len, "Write loop finished but not all bytes written?");
     result = FS_SUCCESS; // Mark success if loop completed
 
 cleanup_write:
     // --- 7. Update File Offset and Size ---
     serial_write("[FAT IO TRACE] fat_write_internal: Write loop finished.\n");
     serial_write("  Result=0x"); serial_print_hex((uint32_t)result);
     serial_write("  TotalWritten=0x"); serial_print_hex((uint32_t)total_bytes_written); serial_write("\n");
 
     irq_flags = spinlock_acquire_irqsave(&fs->lock); // Lock for final context update
     off_t final_offset = current_offset + total_bytes_written;
     file->offset = final_offset; // Update VFS file handle offset
 
     if ((uint64_t)final_offset > file_size_before_write) {
         serial_write("[FAT IO DEBUG] fat_write_internal: Updating file size\n");
         fctx->file_size = (uint32_t)final_offset;
         file_metadata_changed = true; // Mark that metadata changed
 
         // ********** FIX 2: Update Dir Entry Size **********
         serial_write("[FAT IO DEBUG] fat_write_internal: Immediately updating directory entry size...\n");
         int rc = update_directory_entry_size_now(fs, fctx); // Assumes lock is held
         if (rc != FS_SUCCESS) {
             // *** CORRECTED LOGGING CALL ***
             terminal_printf("[FAT IO ERROR] fat_write_internal: Failed to update dir entry size! (err %d)\n", rc);
             // ******************************
             if (result == FS_SUCCESS) result = rc;
         } else {
             serial_write("[FAT IO DEBUG] fat_write_internal: Directory entry size updated.\n");
         }
         // ********** END OF FIX 2 **********
 
     }
     // Always mark dirty if metadata changed, as timestamps might need update on close
     if (file_metadata_changed) {
         fctx->dirty = true;
         serial_write("[FAT IO DEBUG] fat_write_internal: Marked context dirty due to metadata change.\n");
     }
 
     // TODO: Timestamp update logic would go here and set fctx->dirty = true;
 
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     serial_write("[FAT IO TRACE] fat_write_internal: Exit\n");
     serial_write("  ReturnValue=0x"); serial_print_hex((uint32_t)((result < 0) ? result : (int)total_bytes_written)); serial_write("\n");
     return (result < 0) ? result : (int)total_bytes_written; // Return bytes written or negative error
 }
 


/* --- fat_lseek_internal --- (Unchanged from previous version) */
off_t fat_lseek_internal(file_t *file, off_t offset, int whence) {
    if (!file || !file->vnode || !file->vnode->data) { return (off_t)FS_ERR_BAD_F; }
    fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
    KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fctx->fs->lock);
    off_t file_size = (off_t)fctx->file_size;
    spinlock_release_irqrestore(&fctx->fs->lock, irq_flags);

    off_t current_offset = file->offset;
    off_t new_offset;

    serial_write("[FAT IO TRACE] fat_lseek_internal: Enter\n");
    serial_write("  CurrentOffset=0x"); serial_print_hex((uint32_t)current_offset);
    serial_write("  FileSize=0x"); serial_print_hex((uint32_t)file_size);
    serial_write("  ReqOffset=0x"); serial_print_hex((uint32_t)offset);
    serial_write("  Whence=0x"); serial_print_hex((uint32_t)whence); serial_write("\n");

    switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR:
            if ((offset > 0 && current_offset > (LONG_MAX - offset)) || (offset < 0 && current_offset < (LONG_MIN - offset))) { serial_write("[FAT IO ERROR] fat_lseek_internal: SEEK_CUR overflow\n"); return (off_t)FS_ERR_OVERFLOW; }
            new_offset = current_offset + offset;
            break;
        case SEEK_END:
            if ((offset > 0 && file_size > (LONG_MAX - offset)) || (offset < 0 && file_size < (LONG_MIN - offset))) { serial_write("[FAT IO ERROR] fat_lseek_internal: SEEK_END overflow\n"); return (off_t)FS_ERR_OVERFLOW; }
            new_offset = file_size + offset;
            break;
        default: serial_write("[FAT IO ERROR] fat_lseek_internal: Invalid whence value\n"); serial_write("  Whence=0x"); serial_print_hex((uint32_t)whence); serial_write("\n"); return (off_t)FS_ERR_INVALID_PARAM;
    }

    if (new_offset < 0) { serial_write("[FAT IO ERROR] fat_lseek_internal: Resulting offset is negative\n"); serial_write("  NewOffset=0x"); serial_print_hex((uint32_t)new_offset); serial_write("\n"); return (off_t)FS_ERR_INVALID_PARAM; }

    file->offset = new_offset;

    serial_write("[FAT IO TRACE] fat_lseek_internal: Exit\n");
    serial_write("  NewOffset=0x"); serial_print_hex((uint32_t)new_offset); serial_write("\n");
    return new_offset;
}


/**
 * @brief Updates only the first cluster field of a directory entry on disk immediately.
 * Used after allocating the *first* data cluster for a file during write.
 * Assumes the caller holds the filesystem lock (fs->lock).
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param fctx Pointer to the file context containing the updated first_cluster and dir entry location.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
 int update_directory_entry_first_cluster_now(fat_fs_t *fs, fat_file_context_t *fctx) {
    KERNEL_ASSERT(fs != NULL && fctx != NULL, "NULL context in update_directory_entry_first_cluster_now");
    KERNEL_ASSERT(fctx->first_cluster >= 2, "Invalid first cluster value in context"); // Should have been allocated

    // Use FORMATTED logging if terminal.h is included, otherwise basic serial logs
    terminal_printf("[FAT IO UpdateCluster] Updating on-disk entry for cluster %lu at DirClu=%lu, DirOff=%lu\n",
                   (unsigned long)fctx->first_cluster,
                   (unsigned long)fctx->dir_entry_cluster,
                   (unsigned long)fctx->dir_entry_offset);

    size_t sector_size = fs->bytes_per_sector;
    uint32_t sector_offset_in_chain = fctx->dir_entry_offset / sector_size;
    size_t offset_in_sector = fctx->dir_entry_offset % sector_size;

    if (offset_in_sector % sizeof(fat_dir_entry_t) != 0) {
         terminal_printf("[FAT IO ERROR] Directory entry offset %lu is misaligned!\n", (unsigned long)fctx->dir_entry_offset);
         return FS_ERR_INVALID_PARAM; // Or internal error
    }

    uint32_t lba;
    int ret = FS_SUCCESS;

    // --- Determine LBA of the directory sector ---
    // Note: This relies on read_directory_sector or similar logic.
    // Since read_directory_sector is in fat_dir.c, we either duplicate its LBA finding logic
    // or (better) call it. We included fat_dir.h for this purpose.

    uint8_t *temp_sector_buf = kmalloc(sector_size);
    if (!temp_sector_buf) return FS_ERR_OUT_OF_MEMORY;

    // Need to read the sector first to get the correct LBA *and* to modify it
    ret = read_directory_sector(fs, fctx->dir_entry_cluster, sector_offset_in_chain, temp_sector_buf);
    if (ret != FS_SUCCESS) {
        terminal_printf("[FAT IO UpdateCluster] Failed to read directory sector (err %d)\n", ret);
        kfree(temp_sector_buf);
        return ret;
    }

    // Calculate LBA *after* successfully reading (read_directory_sector doesn't return LBA)
    // Re-calculate LBA finding logic here (simplified version from read_directory_sector):
    if (fctx->dir_entry_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
        if (sector_offset_in_chain >= fs->root_dir_sectors) { kfree(temp_sector_buf); return FS_ERR_INVALID_PARAM; }
        lba = fs->root_dir_start_lba + sector_offset_in_chain;
    } else if (fctx->dir_entry_cluster >= 2) {
        uint32_t current_cluster = fctx->dir_entry_cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
        uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_cluster, &next_cluster); // Needs fat_utils.h
            if (ret != FS_SUCCESS) { kfree(temp_sector_buf); return ret; }
            if (next_cluster >= fs->eoc_marker) { kfree(temp_sector_buf); return FS_ERR_INVALID_PARAM; }
            current_cluster = next_cluster;
        }
        lba = fat_cluster_to_lba(fs, current_cluster); // Needs fat_utils.h
        if (lba == 0) { kfree(temp_sector_buf); return FS_ERR_IO; }
        lba += sector_in_final_cluster;
    } else {
        kfree(temp_sector_buf);
        return FS_ERR_INVALID_PARAM;
    }

    // --- Modify and Write Back the directory sector ---
    terminal_printf("[FAT IO UpdateCluster] Modifying directory sector buffer for LBA %lu\n", (unsigned long)lba);
    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) {
        terminal_printf("[FAT IO UpdateCluster] Failed to get buffer for LBA %lu\n", (unsigned long)lba);
        kfree(temp_sector_buf);
        return FS_ERR_IO;
    }

    // Locate the entry within the buffer
    fat_dir_entry_t* entry_in_buffer = (fat_dir_entry_t*)(b->data + offset_in_sector);

    // Update only the cluster fields
    entry_in_buffer->first_cluster_low = (uint16_t)(fctx->first_cluster & 0xFFFF);
    entry_in_buffer->first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
    terminal_printf("[FAT IO UpdateCluster] Updated entry in buffer: Cluster set to %lu\n", (unsigned long)fctx->first_cluster);

    // Mark buffer dirty and release
    buffer_mark_dirty(b);
    buffer_release(b);
    kfree(temp_sector_buf); // Free the temporary buffer used for read

    terminal_printf("[FAT IO UpdateCluster] Directory entry first cluster updated successfully on disk (via buffer cache).\n");
    return FS_SUCCESS;
}


/**
 * @brief Updates only the file size field of a directory entry on disk immediately.
 * Used after a write operation extends the file size.
 * Assumes the caller holds the filesystem lock (fs->lock).
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param fctx Pointer to the file context containing the updated file_size and dir entry location.
 * @return FS_SUCCESS on success, negative error code on failure.
 */
 int update_directory_entry_size_now(fat_fs_t *fs, fat_file_context_t *fctx) {
    KERNEL_ASSERT(fs != NULL && fctx != NULL, "NULL context in update_directory_entry_size_now");

    // Use FORMATTED logging if terminal.h is included, otherwise basic serial logs
    terminal_printf("[FAT IO UpdateSize] Updating on-disk entry size to %lu at DirClu=%lu, DirOff=%lu\n",
                   (unsigned long)fctx->file_size,
                   (unsigned long)fctx->dir_entry_cluster,
                   (unsigned long)fctx->dir_entry_offset);

    size_t sector_size = fs->bytes_per_sector;
    uint32_t sector_offset_in_chain = fctx->dir_entry_offset / sector_size;
    size_t offset_in_sector = fctx->dir_entry_offset % sector_size;

    if (offset_in_sector % sizeof(fat_dir_entry_t) != 0) {
         terminal_printf("[FAT IO ERROR] Directory entry offset %lu is misaligned!\n", (unsigned long)fctx->dir_entry_offset);
         return FS_ERR_INVALID_PARAM;
    }

    uint32_t lba;
    int ret = FS_SUCCESS;

    // --- Determine LBA of the directory sector ---
    // (Duplicate logic from update_directory_entry_first_cluster_now)
    uint8_t *temp_sector_buf = kmalloc(sector_size);
    if (!temp_sector_buf) return FS_ERR_OUT_OF_MEMORY;

    ret = read_directory_sector(fs, fctx->dir_entry_cluster, sector_offset_in_chain, temp_sector_buf);
    if (ret != FS_SUCCESS) {
        terminal_printf("[FAT IO UpdateSize] Failed to read directory sector (err %d)\n", ret);
        kfree(temp_sector_buf);
        return ret;
    }

    // Calculate LBA after successful read
    if (fctx->dir_entry_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
        if (sector_offset_in_chain >= fs->root_dir_sectors) { kfree(temp_sector_buf); return FS_ERR_INVALID_PARAM; }
        lba = fs->root_dir_start_lba + sector_offset_in_chain;
    } else if (fctx->dir_entry_cluster >= 2) {
        uint32_t current_cluster = fctx->dir_entry_cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
        uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_cluster, &next_cluster);
            if (ret != FS_SUCCESS) { kfree(temp_sector_buf); return ret; }
            if (next_cluster >= fs->eoc_marker) { kfree(temp_sector_buf); return FS_ERR_INVALID_PARAM; }
            current_cluster = next_cluster;
        }
        lba = fat_cluster_to_lba(fs, current_cluster);
        if (lba == 0) { kfree(temp_sector_buf); return FS_ERR_IO; }
        lba += sector_in_final_cluster;
    } else {
        kfree(temp_sector_buf);
        return FS_ERR_INVALID_PARAM;
    }
    kfree(temp_sector_buf); // Free temp buffer now that LBA is calculated

    // --- Read-Modify-Write the directory sector ---
    terminal_printf("[FAT IO UpdateSize] Modifying directory sector buffer for LBA %lu\n", (unsigned long)lba);
    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) {
        terminal_printf("[FAT IO UpdateSize] Failed to get buffer for LBA %lu\n", (unsigned long)lba);
        return FS_ERR_IO;
    }

    // Locate the entry within the buffer
    fat_dir_entry_t* entry_in_buffer = (fat_dir_entry_t*)(b->data + offset_in_sector);

    // Update only the file size field
    entry_in_buffer->file_size = fctx->file_size;
    terminal_printf("[FAT IO UpdateSize] Updated entry in buffer: Size set to %lu\n", (unsigned long)fctx->file_size);

    // Mark buffer dirty and release
    buffer_mark_dirty(b);
    buffer_release(b);

    terminal_printf("[FAT IO UpdateSize] Directory entry size updated successfully on disk (via buffer cache).\n");
    return FS_SUCCESS;
}
