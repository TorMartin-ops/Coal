/**
 * @file fat_io.c
 * @brief File I/O operations implementation for FAT filesystem driver.
 * @author Tor Martin Kohle
 * @version 2.0
 *
 * 
 * - Implemented VFS read and write operations with detailed logging.
 * - Handles cluster chain traversal, allocation on write, EOF, errors.
 * - Uses buffer cache for all cluster/sector I/O.
 * - Includes locking for shared filesystem structure access (FAT table, context).
 * - Updates file size and context dirty flag on write.
 * - Logging now uses basic serial functions and terminal_printf for formatted output.
 * - Allocates the first cluster if writing to an empty file (first_cluster == 0).
 * - Correction: Removed inappropriate call to flush_fat_table from close.
 * - Correction: Fixed incorrect usage of serial_write for formatted logging.
 * - TODO: Timestamp updates on write are currently disabled/placeholders.
 */

// --- Core Includes ---
#include <kernel/fs/fat/fat_io.h>
#include <kernel/fs/fat/fat_core.h>       // Defines fat_fs_t, fat_file_context_t, FAT_TYPE_*, FAT_ATTR_*, FAT_ATTR_LONG_NAME, fat_dir_entry_t etc.
#include <kernel/fs/fat/fat_utils.h>      // fat_cluster_to_lba, fat_get_current_timestamp (placeholder)
#include <kernel/fs/fat/fat_alloc.h>      // fat_get_next_cluster, fat_allocate_cluster
#include <kernel/fs/fat/fat_dir.h>        // update_directory_entry (needed for close/flush), read_directory_sector (used in close)
#include <kernel/drivers/storage/buffer_cache.h>   // buffer_get, buffer_release, buffer_mark_dirty
#include <kernel/sync/spinlock.h>       // spinlock_t, spinlock_acquire_irqsave, spinlock_release_irqrestore
#include <kernel/drivers/display/serial.h>         // serial_write, serial_print_hex
#include <kernel/fs/vfs/sys_file.h>       // O_* flags, SEEK_* defines
#include <kernel/memory/kmalloc.h>        // Kernel memory allocation (kfree - needed by helpers)
#include <kernel/fs/vfs/fs_errno.h>       // Filesystem error codes
#include <kernel/lib/string.h>         // memcpy, memset
#include <kernel/lib/assert.h>         // KERNEL_ASSERT
#include <libc/limits.h>    // LONG_MAX, LONG_MIN, SIZE_MAX
#include <libc/stddef.h>    // NULL, size_t
#include <libc/stdbool.h>   // bool
#include <libc/stdarg.h>    // varargs for printf (though not used in macros now)
#include <kernel/drivers/timer/time.h>           // kernel_get_time(), kernel_time_t (placeholder)
#include <kernel/drivers/display/terminal.h>       // terminal_printf FOR FORMATTED LOGGING

/* --- Helper Macros --- */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* --- Cluster I/O Helpers --- */

/**
 * @brief Reads a block of data from a specific cluster or FAT12/16 root dir area via buffer cache.
 */
int read_cluster_cached(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_location, void *buf, size_t len)
{
    KERNEL_ASSERT(fs != NULL && buf != NULL, "NULL fs or buf");
    KERNEL_ASSERT(len > 0, "Zero length read");

    uint32_t sector_size = fs->bytes_per_sector;
    uint32_t location_size;
    uint32_t start_lba;

    if (cluster == 0 && (fs->type == FAT_TYPE_FAT12 || fs->type == FAT_TYPE_FAT16)) { // FAT12/16 Root Directory
        location_size = fs->root_dir_sectors * fs->bytes_per_sector;
        start_lba = fs->root_dir_start_lba;
        KERNEL_ASSERT(offset_in_location < location_size, "Offset out of root dir bounds");
        KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of root dir bounds");
        // serial_write("[FAT_IO] Reading FAT12/16 Root Dir, LBA: 0x"); serial_print_hex(start_lba); serial_write("\n");
    } else if (cluster >= 2) { // Data Cluster
        location_size = fs->cluster_size_bytes;
        start_lba = fat_cluster_to_lba(fs, cluster);
        if (start_lba == 0) {
            serial_printf("[FAT_IO_ERR] read_cluster_cached: Invalid LBA for cluster 0x%lx\n", (unsigned long)cluster);
            return FS_ERR_IO;
        }
        KERNEL_ASSERT(offset_in_location < location_size, "Offset out of cluster bounds");
        KERNEL_ASSERT(offset_in_location + len <= location_size, "Length out of cluster bounds");
        // serial_write("[FAT_IO] Reading Cluster: 0x"); serial_print_hex(cluster); serial_write(", LBA: 0x"); serial_print_hex(start_lba); serial_write("\n");
    } else {
        serial_printf("[FAT_IO_ERR] read_cluster_cached: Invalid cluster 0x%lx for read\n", (unsigned long)cluster);
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
        // serial_write("[FAT_IO] Reading LBA: 0x"); serial_print_hex(current_lba); serial_write("\n");

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
        if (!b) {
            serial_printf("[FAT_IO_ERR] read_cluster_cached: Buffer get failed for LBA 0x%lx\n", (unsigned long)current_lba);
            return FS_ERR_IO;
        }

        size_t offset_within_this_sector = (sec_idx == start_sector_in_location) ? (offset_in_location % sector_size) : 0;
        size_t bytes_to_copy_from_this_sector = MIN(sector_size - offset_within_this_sector, len - bytes_read_total);

        memcpy(dest_ptr, b->data + offset_within_this_sector, bytes_to_copy_from_this_sector);
        buffer_release(b);

        dest_ptr += bytes_to_copy_from_this_sector;
        bytes_read_total += bytes_to_copy_from_this_sector;
    }

    KERNEL_ASSERT(bytes_read_total == len, "Bytes read mismatch");
    // serial_write("[FAT_IO] read_cluster_cached: Success. Cluster: 0x"); serial_print_hex(cluster); serial_write(", Bytes: 0x"); serial_print_hex((uint32_t)len); serial_write("\n");
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
        serial_printf("[FAT_IO_ERR] write_cluster_cached: Invalid LBA for cluster 0x%lx\n", (unsigned long)cluster);
        return FS_ERR_IO;
    }
    // serial_write("[FAT_IO] Writing Cluster: 0x"); serial_print_hex(cluster); serial_write(", LBA: 0x"); serial_print_hex(cluster_lba); serial_write(", Len: 0x"); serial_print_hex((uint32_t)len); serial_write("\n");

    size_t bytes_written_total = 0;
    const uint8_t *src_ptr = (const uint8_t *)buf;
    int result = FS_SUCCESS;

    for (uint32_t sec_idx = start_sector_in_cluster; sec_idx <= end_sector_in_cluster; sec_idx++) {
        uint32_t current_lba = cluster_lba + sec_idx;
        // serial_write("[FAT_IO] Writing LBA: 0x"); serial_print_hex(current_lba); serial_write("\n");

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, current_lba);
        if (!b) {
            serial_printf("[FAT_IO_ERR] write_cluster_cached: Buffer get failed for LBA 0x%lx\n", (unsigned long)current_lba);
            result = FS_ERR_IO; goto write_cluster_cleanup;
        }

        size_t offset_within_this_sector = (sec_idx == start_sector_in_cluster) ? (offset_in_cluster % sector_size) : 0;
        size_t bytes_to_copy_to_this_sector = MIN(sector_size - offset_within_this_sector, len - bytes_written_total);

        memcpy(b->data + offset_within_this_sector, src_ptr, bytes_to_copy_to_this_sector);
        buffer_mark_dirty(b);
        buffer_release(b);

        src_ptr += bytes_to_copy_to_this_sector;
        bytes_written_total += bytes_to_copy_to_this_sector;
    }

write_cluster_cleanup:
    if (result != FS_SUCCESS) {
        serial_printf("[FAT_IO_ERR] write_cluster_cached: Error %d during write to cluster 0x%lx. Wrote 0x%x bytes.\n", result, (unsigned long)cluster, (unsigned int)bytes_written_total);
        return result;
    }

    KERNEL_ASSERT(bytes_written_total == len, "Bytes written mismatch");
    // serial_write("[FAT_IO] write_cluster_cached: Success. Cluster: 0x"); serial_print_hex(cluster); serial_write(", Bytes: 0x"); serial_print_hex((uint32_t)len); serial_write("\n");
    return (int)bytes_written_total;
}


/* --- VFS Operation Implementations --- */

/**
 * @brief Reads data from an opened file. Implements VFS read.
 */
int fat_read_internal(file_t *file, void *buf, size_t len)
{
    if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
        serial_write("[FAT_IO_ERR] fat_read: Invalid parameters\n");
        return FS_ERR_INVALID_PARAM;
    }
    if (len == 0) return 0;

    fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
    KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
    fat_fs_t *fs = fctx->fs;

    if (fctx->is_directory) {
        serial_write("[FAT_IO_ERR] fat_read: Cannot read from a directory\n");
        return FS_ERR_IS_A_DIRECTORY;
    }

    uintptr_t irq_flags;
    int result = FS_SUCCESS;
    size_t total_bytes_read = 0;

    irq_flags = spinlock_acquire_irqsave(&fs->lock);
    off_t current_offset = file->offset;
    uint32_t file_size = fctx->file_size;
    uint32_t first_cluster = fctx->first_cluster;
    spinlock_release_irqrestore(&fs->lock, irq_flags);

    // serial_printf("[FAT_IO] fat_read: Offset=0x%llx, ReqLen=0x%zx, FileSize=0x%lx, FirstClu=0x%lx\n", (unsigned long long)current_offset, len, (unsigned long)file_size, (unsigned long)first_cluster);

    if (current_offset < 0) {
        serial_write("[FAT_IO_ERR] fat_read: Negative file offset\n");
        return FS_ERR_INVALID_PARAM;
    }
    if ((uint64_t)current_offset >= file_size) { // At or beyond EOF
        return 0;
    }

    size_t max_readable = MIN(len, (size_t)((uint64_t)file_size - current_offset));
    if (max_readable == 0) return 0;
    len = max_readable;
    // serial_write("[FAT_IO] fat_read: Adjusted read length to 0x"); serial_print_hex((uint32_t)len); serial_write("\n");

    size_t cluster_size = fs->cluster_size_bytes;
    if (cluster_size == 0) { serial_write("[FAT_IO_ERR] fat_read: Invalid cluster size 0\n"); return FS_ERR_INVALID_FORMAT; }
    
    // If file has size but no cluster, or has no first cluster, it's either empty or corrupt.
    if (first_cluster < 2) {
        if (file_size > 0) {
            serial_write("[FAT_IO_ERR] fat_read: File size > 0 but first cluster invalid\n");
            return FS_ERR_CORRUPT;
        }
        return 0; // Empty file, nothing to read
    }

    uint32_t current_cluster_num = first_cluster;
    uint32_t cluster_index_to_seek = (uint32_t)(current_offset / cluster_size);
    uint32_t offset_in_first_read_cluster = (uint32_t)(current_offset % cluster_size);

    // Traverse cluster chain to the starting cluster for the read
    for (uint32_t i = 0; i < cluster_index_to_seek; i++) {
        uint32_t next_cluster;
        irq_flags = spinlock_acquire_irqsave(&fs->lock);
        result = fat_get_next_cluster(fs, current_cluster_num, &next_cluster);
        spinlock_release_irqrestore(&fs->lock, irq_flags);

        if (result != FS_SUCCESS) {
            serial_printf("[FAT_IO_ERR] fat_read: Seek failed getting next cluster from 0x%lx\n", (unsigned long)current_cluster_num);
            return FS_ERR_IO;
        }
        if (next_cluster >= fs->eoc_marker) {
            serial_write("[FAT_IO_ERR] fat_read: Seek failed - EOC found prematurely\n");
            return FS_ERR_CORRUPT;
        }
        current_cluster_num = next_cluster;
    }
    // serial_write("[FAT_IO] fat_read: Seeked to StartClu=0x"); serial_print_hex(current_cluster_num); serial_write(", OffsetInClu=0x"); serial_print_hex(offset_in_first_read_cluster); serial_write("\n");

    // Read data cluster by cluster
    uint32_t current_offset_in_cluster = offset_in_first_read_cluster;
    while (total_bytes_read < len) {
        if (current_cluster_num < 2 || current_cluster_num >= fs->eoc_marker) {
            serial_printf("[FAT_IO_ERR] fat_read: Invalid cluster 0x%lx in read loop\n", (unsigned long)current_cluster_num);
            result = FS_ERR_CORRUPT; goto cleanup_read;
        }

        size_t bytes_to_read_this_cluster = MIN(cluster_size - current_offset_in_cluster, len - total_bytes_read);
        // serial_printf("[FAT_IO] fat_read: Reading 0x%zx bytes from Clu=0x%lx, Offset=0x%lx\n", bytes_to_read_this_cluster, (unsigned long)current_cluster_num, (unsigned long)current_offset_in_cluster);

        result = read_cluster_cached(fs, current_cluster_num, current_offset_in_cluster, (uint8_t*)buf + total_bytes_read, bytes_to_read_this_cluster);

        if (result < 0) {
            serial_printf("[FAT_IO_ERR] fat_read: read_cluster_cached failed with %d\n", result);
            goto cleanup_read;
        }
        if ((size_t)result != bytes_to_read_this_cluster) {
            serial_write("[FAT_IO_ERR] fat_read: Short read from read_cluster_cached\n");
            result = FS_ERR_IO; goto cleanup_read;
        }

        total_bytes_read += bytes_to_read_this_cluster;
        current_offset_in_cluster = 0; // Subsequent reads from a cluster start at its beginning

        if (total_bytes_read < len) { // Need to move to the next cluster
            uint32_t next_cluster;
            irq_flags = spinlock_acquire_irqsave(&fs->lock);
            result = fat_get_next_cluster(fs, current_cluster_num, &next_cluster);
            spinlock_release_irqrestore(&fs->lock, irq_flags);

            if (result != FS_SUCCESS) {
                serial_printf("[FAT_IO_ERR] fat_read: Failed to get next cluster after 0x%lx\n", (unsigned long)current_cluster_num);
                result = FS_ERR_IO; goto cleanup_read;
            }
            current_cluster_num = next_cluster;
            if (current_cluster_num >= fs->eoc_marker) { // Reached EOC marker
                // This means we've read all allocated clusters, but `len` (derived from file_size) might have expected more.
                // This implies a mismatch between file_size and actual chain length.
                // However, we should return the bytes read so far as per file_size and EOF logic handled earlier.
                // If `len` was capped by `file_size - current_offset`, then `total_bytes_read` should equal `len` if no error occurred.
                // If we hit EOC before `total_bytes_read == len`, it means the file is shorter than its metadata `file_size` indicates.
                serial_printf("[FAT_IO_WARN] fat_read: EOC 0x%lx reached mid-read; file might be corrupt or size mismatch. Read 0x%zx of 0x%zx bytes.\n", (unsigned long)current_cluster_num, total_bytes_read, len);
                break; 
            }
        }
    }
    result = FS_SUCCESS; // If loop completed or broke due to EOC (which is not an error for read itself)

cleanup_read:
    // serial_printf("[FAT_IO] fat_read: Exit. TotalRead=0x%zx, Result=%d\n", total_bytes_read, result);
    return (result < 0) ? result : (int)total_bytes_read;
}


/**
 * @brief Closes an opened file. Updates directory entry if modified.
 */
int fat_close_internal(file_t *file)
{
    if (!file || !file->vnode || !file->vnode->data) { return FS_ERR_BAD_F; }
    fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
    KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
    fat_fs_t *fs = fctx->fs;

    // serial_printf("[FAT_IO] fat_close: Closing fctx=0x%p, dirty=0x%x\n", fctx, (unsigned int)fctx->dirty);

    int update_result = FS_SUCCESS;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);

    if (fctx->dirty) {
        // serial_printf("[FAT_IO] fat_close: Context dirty. Updating dir entry (DirClu=0x%lx, DirOff=0x%lx)\n", (unsigned long)fctx->dir_entry_cluster, (unsigned long)fctx->dir_entry_offset);
        
        // This part updates the directory entry on disk with new size, first cluster, and timestamps.
        // It's crucial for data integrity.
        fat_dir_entry_t existing_entry; // Temporary stack storage
        uint8_t *sector_buf = kmalloc(fs->bytes_per_sector); // Buffer for sector read/write
        if (!sector_buf) {
            serial_write("[FAT_IO_ERR] fat_close: Failed to alloc sector_buf for dir update\n");
            update_result = FS_ERR_OUT_OF_MEMORY;
        } else {
            // Read the sector containing the directory entry
            int read_sec_res = read_directory_sector(fs, fctx->dir_entry_cluster, fctx->dir_entry_offset / fs->bytes_per_sector, sector_buf);
            if (read_sec_res == FS_SUCCESS) {
                memcpy(&existing_entry, sector_buf + (fctx->dir_entry_offset % fs->bytes_per_sector), sizeof(fat_dir_entry_t));

                // Update fields from context
                existing_entry.file_size = fctx->file_size;
                existing_entry.first_cluster_low = (uint16_t)(fctx->first_cluster & 0xFFFF);
                existing_entry.first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
                
                // TODO: Update timestamps (write_time, write_date, last_access_date)
                // fat_get_current_timestamp(&existing_entry.write_time, &existing_entry.write_date);
                // existing_entry.last_access_date = existing_entry.write_date; // Or specific access date logic

                // Write the modified entry back
                update_result = update_directory_entry(fs, fctx->dir_entry_cluster, fctx->dir_entry_offset, &existing_entry);
                if (update_result == FS_SUCCESS) {
                    fctx->dirty = false; // Clear dirty flag ONLY on successful write-back
                    // serial_write("[FAT_IO] fat_close: Directory entry update successful.\n");
                } else {
                    serial_printf("[FAT_IO_ERR] fat_close: Failed to update directory entry (err %d)\n", update_result);
                }
            } else {
                serial_printf("[FAT_IO_ERR] fat_close: Failed to read dir sector for update (err %d)\n", read_sec_res);
                update_result = read_sec_res;
            }
            kfree(sector_buf);
        }
    }
    spinlock_release_irqrestore(&fs->lock, irq_flags);

    kfree(fctx);
    file->vnode->data = NULL;

    // serial_printf("[FAT_IO] fat_close: Exit. Result=%d\n", update_result);
    return update_result;
}


/**
 * @brief Writes data to an opened file. Implements VFS write.
 * Handles cluster allocation, EOF extension, and updating file metadata.
 */
int fat_write_internal(file_t *file, const void *buf, size_t len)
{
    if (!file || !file->vnode || !file->vnode->data || (!buf && len > 0)) {
        serial_write("[FAT_IO_ERR] fat_write: Invalid parameters\n");
        return FS_ERR_INVALID_PARAM;
    }
    if (len == 0) return 0;

    fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
    KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");
    fat_fs_t *fs = fctx->fs;

    if (fctx->is_directory) {
        serial_write("[FAT_IO_ERR] fat_write: Cannot write to a directory\n");
        return FS_ERR_IS_A_DIRECTORY;
    }
    if (!(file->flags & (O_WRONLY | O_RDWR))) {
        serial_write("[FAT_IO_ERR] fat_write: File not opened for writing\n");
        return FS_ERR_PERMISSION_DENIED;
    }

    uintptr_t irq_flags;
    int result = FS_SUCCESS;
    size_t total_bytes_written = 0;
    bool file_metadata_changed = false; // Tracks if first_cluster or file_size changes

    // Determine write position
    irq_flags = spinlock_acquire_irqsave(&fs->lock);
    off_t current_offset = file->offset;
    uint32_t file_size_before_write = fctx->file_size;
    uint32_t first_cluster_before_write = fctx->first_cluster; // For checking if it's newly allocated
    
    if (file->flags & O_APPEND) { current_offset = (off_t)file_size_before_write; }
    
    if (current_offset < 0) {
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        serial_write("[FAT_IO_ERR] fat_write: Negative file offset\n");
        return FS_ERR_INVALID_PARAM;
    }
    uint32_t current_first_cluster = fctx->first_cluster; // Use this for the rest of the write logic
    spinlock_release_irqrestore(&fs->lock, irq_flags);

    // serial_printf("[FAT_IO] fat_write: Offset=0x%llx, ReqLen=0x%zx, FileSize=0x%lx, FirstClu=0x%lx\n", (unsigned long long)current_offset, len, (unsigned long)file_size_before_write, (unsigned long)current_first_cluster);

    size_t cluster_size = fs->cluster_size_bytes;
    if (cluster_size == 0) {
        serial_write("[FAT_IO_ERR] fat_write: Invalid cluster size 0\n");
        return FS_ERR_INVALID_FORMAT;
    }

    // Allocate initial cluster if file is empty and being written to at offset 0
    if (current_first_cluster < 2) {
        if (current_offset != 0) { // Cannot write to an offset in an unallocated file
            serial_write("[FAT_IO_ERR] fat_write: Attempt to write at offset in empty, unallocated file\n");
            return FS_ERR_INVALID_PARAM;
        }
        // serial_printf("[FAT_IO] fat_write: Allocating initial cluster for empty file (Offset: %lld).\n", (long long)current_offset);
        irq_flags = spinlock_acquire_irqsave(&fs->lock);
        uint32_t new_cluster = fat_allocate_cluster(fs, 0); // Allocate and mark as EOC
        if (new_cluster < 2) {
            spinlock_release_irqrestore(&fs->lock, irq_flags);
            serial_write("[FAT_IO_ERR] fat_write: Failed to allocate initial cluster (no space?)\n");
            return FS_ERR_NO_SPACE;
        }
        fctx->first_cluster = new_cluster;
        current_first_cluster = new_cluster; // Update local working copy
        file_metadata_changed = true;
        fctx->dirty = true; // Context is dirty due to new first cluster

        // Immediately update the directory entry with the new first cluster.
        // This is crucial to prevent data loss if system crashes before close.
        int rc_update_clu = update_directory_entry_first_cluster_now(fs, fctx);
        if (rc_update_clu != FS_SUCCESS) {
            serial_printf("[FAT_IO_ERR] fat_write: Failed to update dir entry for new first_cluster %lu! Rolling back.\n", (unsigned long)new_cluster);
            fat_free_cluster_chain(fs, new_cluster); // Free the just-allocated cluster
            fctx->first_cluster = 0;                 // Revert context
            current_first_cluster = 0;
            spinlock_release_irqrestore(&fs->lock, irq_flags);
            return rc_update_clu;
        }
        // serial_printf("[FAT_IO] fat_write: Allocated initial cluster 0x%lx and updated dir entry.\n", (unsigned long)new_cluster);
        spinlock_release_irqrestore(&fs->lock, irq_flags);
    }
    KERNEL_ASSERT(current_first_cluster >= 2 || len == 0, "First cluster invalid after initial check/alloc for non-zero write");


    // Traverse/Extend cluster chain to the starting cluster for the write
    uint32_t current_cluster_num = current_first_cluster;
    uint32_t cluster_index_to_seek = (uint32_t)(current_offset / cluster_size);
    uint32_t offset_in_first_write_cluster = (uint32_t)(current_offset % cluster_size);

    for (uint32_t i = 0; i < cluster_index_to_seek; i++) {
        uint32_t next_cluster;
        bool allocated_new_in_seek __attribute__((unused)) = false;
        
        irq_flags = spinlock_acquire_irqsave(&fs->lock);
        int find_result = fat_get_next_cluster(fs, current_cluster_num, &next_cluster);
        if (find_result != FS_SUCCESS) {
            spinlock_release_irqrestore(&fs->lock, irq_flags);
            serial_printf("[FAT_IO_ERR] fat_write: Seek/Extend: Error getting next cluster from 0x%lx\n", (unsigned long)current_cluster_num);
            result = FS_ERR_IO; goto cleanup_write;
        }

        if (next_cluster >= fs->eoc_marker) { // Need to allocate a new cluster
            // serial_printf("[FAT_IO] fat_write: Seek/Extend: Allocating new cluster after 0x%lx\n", (unsigned long)current_cluster_num);
            next_cluster = fat_allocate_cluster(fs, current_cluster_num); // Allocates AND links
            if (next_cluster < 2) {
                spinlock_release_irqrestore(&fs->lock, irq_flags);
                serial_write("[FAT_IO_ERR] fat_write: Seek/Extend: Failed to allocate cluster (no space?)\n");
                result = FS_ERR_NO_SPACE; goto cleanup_write;
            }
            fctx->dirty = true; // FAT chain changed
            file_metadata_changed = true; // File structure changed
            allocated_new_in_seek = true;
        }
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        current_cluster_num = next_cluster;
        // if (allocated_new_in_seek) serial_printf("[FAT_IO] fat_write: Seek/Extend: Allocated new cluster 0x%lx\n", (unsigned long)current_cluster_num);
    }
    // serial_write("[FAT_IO] fat_write: Seek/Extend successful. StartClu=0x"); serial_print_hex(current_cluster_num); serial_write(", OffsetInClu=0x"); serial_print_hex(offset_in_first_write_cluster); serial_write("\n");

    // Write data cluster by cluster, allocating as needed
    uint32_t current_offset_in_cluster = offset_in_first_write_cluster;
    while (total_bytes_written < len) {
        if (current_cluster_num < 2 || current_cluster_num >= fs->eoc_marker) {
            // This should not happen if allocation logic is correct
            serial_printf("[FAT_IO_ERR] fat_write: Corrupt state - invalid cluster 0x%lx in write loop.\n", (unsigned long)current_cluster_num);
            result = FS_ERR_CORRUPT; goto cleanup_write;
        }

        size_t bytes_to_write_this_cluster = MIN(cluster_size - current_offset_in_cluster, len - total_bytes_written);
        // serial_printf("[FAT_IO] fat_write: Writing 0x%zx bytes to Clu=0x%lx, Offset=0x%lx\n", bytes_to_write_this_cluster, (unsigned long)current_cluster_num, (unsigned long)current_offset_in_cluster);

        int write_res = write_cluster_cached(fs, current_cluster_num, current_offset_in_cluster, (const uint8_t*)buf + total_bytes_written, bytes_to_write_this_cluster);
        if (write_res < 0) {
            serial_printf("[FAT_IO_ERR] fat_write: write_cluster_cached failed with %d\n", write_res);
            result = write_res; goto cleanup_write;
        }
        if ((size_t)write_res != bytes_to_write_this_cluster) {
            serial_write("[FAT_IO_ERR] fat_write: Short write from write_cluster_cached\n");
            result = FS_ERR_IO; goto cleanup_write;
        }

        total_bytes_written += bytes_to_write_this_cluster;
        current_offset_in_cluster = 0; // Subsequent writes to this cluster start at its beginning

        if (total_bytes_written < len) { // Need to get/allocate next cluster
            uint32_t next_cluster;
            bool allocated_new_in_loop __attribute__((unused)) = false;
            int alloc_res = FS_SUCCESS;
            
            irq_flags = spinlock_acquire_irqsave(&fs->lock);
            int find_res = fat_get_next_cluster(fs, current_cluster_num, &next_cluster);
            if (find_res == FS_SUCCESS && next_cluster >= fs->eoc_marker) { // End of chain, need to allocate
                // serial_printf("[FAT_IO] fat_write: Allocating next cluster after 0x%lx (EOC found)\n", (unsigned long)current_cluster_num);
                next_cluster = fat_allocate_cluster(fs, current_cluster_num);
                if (next_cluster < 2) {
                    alloc_res = FS_ERR_NO_SPACE;
                    serial_write("[FAT_IO_ERR] fat_write: Failed to allocate next cluster (no space?)\n");
                } else {
                    fctx->dirty = true;
                    file_metadata_changed = true;
                    allocated_new_in_loop = true;
                }
            } else if (find_res != FS_SUCCESS) {
                alloc_res = FS_ERR_IO;
                serial_printf("[FAT_IO_ERR] fat_write: Failed to get next cluster after 0x%lx during write loop\n", (unsigned long)current_cluster_num);
            }
            // If find_res == FS_SUCCESS and next_cluster is valid data cluster, we just use it.
            spinlock_release_irqrestore(&fs->lock, irq_flags);

            if (alloc_res != FS_SUCCESS) {
                result = alloc_res;
                goto cleanup_write;
            }
            current_cluster_num = next_cluster;
            // if (allocated_new_in_loop) serial_printf("[FAT_IO] fat_write: Allocated next cluster 0x%lx in loop\n", (unsigned long)current_cluster_num);
        }
    }
    KERNEL_ASSERT(total_bytes_written == len, "Write loop finished but not all bytes written?");
    result = FS_SUCCESS;

cleanup_write:
    // Update file offset and size in context
    irq_flags = spinlock_acquire_irqsave(&fs->lock);
    off_t final_offset = current_offset + total_bytes_written;
    file->offset = final_offset; // Update VFS file handle offset

    if ((uint64_t)final_offset > file_size_before_write) {
        fctx->file_size = (uint32_t)final_offset;
        file_metadata_changed = true; // File size changed

        // Immediately update the directory entry with the new file size.
        // serial_printf("[FAT_IO] fat_write: Updating dir entry size to 0x%lx.\n", (unsigned long)fctx->file_size);
        int rc_update_size = update_directory_entry_size_now(fs, fctx);
        if (rc_update_size != FS_SUCCESS) {
            serial_printf("[FAT IO ERROR] fat_write_internal: Failed to update dir entry size! (err %d)\n", rc_update_size);
            // If write itself was successful, this error becomes the primary error.
            if (result == FS_SUCCESS) result = rc_update_size;
        }
    }

    // If first cluster or file size changed, or if FAT chain was modified, context is dirty.
    if (file_metadata_changed) {
        fctx->dirty = true;
        // serial_write("[FAT_IO] fat_write: Marked context dirty due to metadata change.\n");
    }
    // TODO: Timestamp update logic would go here and set fctx->dirty = true;
    spinlock_release_irqrestore(&fs->lock, irq_flags);

    // serial_printf("[FAT_IO] fat_write: Exit. TotalWritten=0x%zx, Result=%d\n", total_bytes_written, result);
    return (result < 0) ? result : (int)total_bytes_written;
}


/**
 * @brief Sets the file offset for the next read or write operation.
 */
off_t fat_lseek_internal(file_t *file, off_t offset, int whence) {
    if (!file || !file->vnode || !file->vnode->data) { return (off_t)FS_ERR_BAD_F; }
    fat_file_context_t *fctx = (fat_file_context_t*)file->vnode->data;
    KERNEL_ASSERT(fctx->fs != NULL, "FAT context missing FS pointer");

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fctx->fs->lock);
    off_t file_size = (off_t)fctx->file_size; // Read under lock
    spinlock_release_irqrestore(&fctx->fs->lock, irq_flags);

    off_t current_offset = file->offset; // Current offset from file_t, not fctx
    off_t new_offset;

    // serial_printf("[FAT_IO] fat_lseek: CurrentOff=0x%llx, FileSize=0x%llx, ReqOff=0x%llx, Whence=%d\n", (unsigned long long)current_offset, (unsigned long long)file_size, (unsigned long long)offset, whence);

    switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR:
            // Check for overflow before addition
            if ((offset > 0 && current_offset > (LONG_MAX - offset)) || (offset < 0 && current_offset < (LONG_MIN - offset))) {
                serial_write("[FAT_IO_ERR] fat_lseek: SEEK_CUR overflow\n"); return (off_t)FS_ERR_OVERFLOW;
            }
            new_offset = current_offset + offset;
            break;
        case SEEK_END:
            // Check for overflow before addition
            if ((offset > 0 && file_size > (LONG_MAX - offset)) || (offset < 0 && file_size < (LONG_MIN - offset))) {
                serial_write("[FAT_IO_ERR] fat_lseek: SEEK_END overflow\n"); return (off_t)FS_ERR_OVERFLOW;
            }
            new_offset = file_size + offset;
            break;
        default:
            serial_printf("[FAT_IO_ERR] fat_lseek: Invalid whence value %d\n", whence);
            return (off_t)FS_ERR_INVALID_PARAM;
    }

    if (new_offset < 0) { // New offset cannot be negative
        serial_printf("[FAT_IO_ERR] fat_lseek: Resulting offset 0x%llx is negative\n", (unsigned long long)new_offset);
        return (off_t)FS_ERR_INVALID_PARAM;
    }

    file->offset = new_offset; // Update the file's current offset

    // serial_printf("[FAT_IO] fat_lseek: Exit. NewOffset=0x%llx\n", (unsigned long long)new_offset);
    return new_offset;
}


/**
 * @brief Immediately updates the first cluster field of a directory entry on disk.
 * Crucial after allocating the *first* data cluster for a file to ensure persistence.
 * Assumes the caller holds the filesystem lock (fs->lock).
 *
 * @param fs Pointer to the FAT filesystem structure.
 * @param fctx Pointer to the file context (contains dir_entry_cluster, dir_entry_offset, new first_cluster).
 * @return FS_SUCCESS on success, or a negative error code.
 */
int update_directory_entry_first_cluster_now(fat_fs_t *fs, fat_file_context_t *fctx) {
    KERNEL_ASSERT(fs != NULL && fctx != NULL, "NULL fs or fctx in update_directory_entry_first_cluster_now");
    KERNEL_ASSERT(fctx->first_cluster >= 2, "Invalid first_cluster in context for update");

    serial_printf("[FAT_IO_Update] DirEntry: Set FirstCluster=%lu (at DirClu=%lu, DirOff=%lu)\n",
                  (unsigned long)fctx->first_cluster,
                  (unsigned long)fctx->dir_entry_cluster,
                  (unsigned long)fctx->dir_entry_offset);

    size_t sector_size = fs->bytes_per_sector;
    uint32_t dir_sector_idx_in_chain = fctx->dir_entry_offset / sector_size; // Which sector of the directory this entry is in
    size_t offset_in_dir_sector = fctx->dir_entry_offset % sector_size;    // Offset of the entry within that sector

    if (offset_in_dir_sector % sizeof(fat_dir_entry_t) != 0) {
        serial_printf("[FAT_IO_ERR] DirEntry Update: Offset %lu is misaligned!\n", (unsigned long)fctx->dir_entry_offset);
        return FS_ERR_INVALID_PARAM; // Or FS_ERR_CORRUPT if it implies fs corruption
    }

    uint32_t target_lba;
    int ret = FS_SUCCESS;

    // To modify the directory entry, we need its LBA.
    // This requires finding the correct sector of the directory.
    // This logic mirrors parts of how one might read a directory sector.
    // A temporary buffer is NOT needed here as we will get the buffer from cache and modify it directly.
    // However, to calculate LBA, we might need to traverse.
    
    // Calculate LBA of the directory sector containing the entry
    if (fctx->dir_entry_cluster == 0 && fs->type != FAT_TYPE_FAT32) { // FAT12/16 root directory
        if (dir_sector_idx_in_chain >= fs->root_dir_sectors) {
            serial_printf("[FAT_IO_ERR] DirEntry Update: Sector index %lu out of bounds for root dir.\n", (unsigned long)dir_sector_idx_in_chain);
            return FS_ERR_INVALID_PARAM;
        }
        target_lba = fs->root_dir_start_lba + dir_sector_idx_in_chain;
    } else if (fctx->dir_entry_cluster >= 2) { // Subdirectory or FAT32 root directory
        uint32_t current_dir_cluster = fctx->dir_entry_cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = dir_sector_idx_in_chain / sectors_per_cluster;
        uint32_t sector_in_target_cluster = dir_sector_idx_in_chain % sectors_per_cluster;

        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_dir_cluster, &next_cluster); // Assumes fs->lock is held by caller
            if (ret != FS_SUCCESS) {
                 serial_printf("[FAT_IO_ERR] DirEntry Update: Failed to get next cluster from %lu (err %d)\n", (unsigned long)current_dir_cluster, ret);
                 return ret;
            }
            if (next_cluster >= fs->eoc_marker) {
                serial_printf("[FAT_IO_ERR] DirEntry Update: EOC found prematurely traversing dir cluster chain for LBA.\n");
                return FS_ERR_CORRUPT; // Directory chain seems shorter than offset implies
            }
            current_dir_cluster = next_cluster;
        }
        target_lba = fat_cluster_to_lba(fs, current_dir_cluster);
        if (target_lba == 0) {
            serial_printf("[FAT_IO_ERR] DirEntry Update: Failed to convert dir cluster %lu to LBA.\n", (unsigned long)current_dir_cluster);
            return FS_ERR_IO;
        }
        target_lba += sector_in_target_cluster;
    } else { // Invalid dir_entry_cluster
        serial_printf("[FAT_IO_ERR] DirEntry Update: Invalid dir_entry_cluster %lu.\n", (unsigned long)fctx->dir_entry_cluster);
        return FS_ERR_INVALID_PARAM;
    }

    // Now, get the buffer for the target LBA, modify, mark dirty, and release.
    // serial_printf("[FAT_IO_Update] Modifying directory sector at LBA %lu\n", (unsigned long)target_lba);
    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, target_lba);
    if (!b) {
        serial_printf("[FAT_IO_ERR] DirEntry Update: Failed to get buffer for LBA %lu\n", (unsigned long)target_lba);
        return FS_ERR_IO;
    }

    fat_dir_entry_t* entry_in_buffer = (fat_dir_entry_t*)(b->data + offset_in_dir_sector);

    entry_in_buffer->first_cluster_low = (uint16_t)(fctx->first_cluster & 0xFFFF);
    entry_in_buffer->first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
    // Timestamps (like modification time) should also be updated here or in a combined function.
    // For now, only cluster is updated as per function name.

    buffer_mark_dirty(b);
    buffer_release(b); // This will eventually write it to disk.

    // serial_printf("[FAT_IO_Update] DirEntry FirstCluster successfully updated on disk (via cache) for LBA %lu.\n", (unsigned long)target_lba);
    return FS_SUCCESS;
}


/**
 * @brief Immediately updates the file size field of a directory entry on disk.
 * Important after a write operation extends the file, for persistence.
 * Assumes the caller holds the filesystem lock (fs->lock).
 *
 * @param fs Pointer to the FAT filesystem structure.
 * @param fctx Pointer to the file context (contains dir_entry_cluster, dir_entry_offset, new file_size).
 * @return FS_SUCCESS on success, or a negative error code.
 */
int update_directory_entry_size_now(fat_fs_t *fs, fat_file_context_t *fctx) {
    KERNEL_ASSERT(fs != NULL && fctx != NULL, "NULL fs or fctx in update_directory_entry_size_now");

    serial_printf("[FAT_IO_Update] DirEntry: Set FileSize=%lu (at DirClu=%lu, DirOff=%lu)\n",
                  (unsigned long)fctx->file_size,
                  (unsigned long)fctx->dir_entry_cluster,
                  (unsigned long)fctx->dir_entry_offset);
    
    size_t sector_size = fs->bytes_per_sector;
    uint32_t dir_sector_idx_in_chain = fctx->dir_entry_offset / sector_size;
    size_t offset_in_dir_sector = fctx->dir_entry_offset % sector_size;

    if (offset_in_dir_sector % sizeof(fat_dir_entry_t) != 0) {
        serial_printf("[FAT_IO_ERR] DirEntry Update: Offset %lu is misaligned!\n", (unsigned long)fctx->dir_entry_offset);
        return FS_ERR_INVALID_PARAM;
    }

    uint32_t target_lba;
    int ret = FS_SUCCESS;

    // Calculate LBA of the directory sector (logic duplicated from update_directory_entry_first_cluster_now for clarity, could be refactored)
    if (fctx->dir_entry_cluster == 0 && fs->type != FAT_TYPE_FAT32) { // FAT12/16 root directory
        if (dir_sector_idx_in_chain >= fs->root_dir_sectors) { return FS_ERR_INVALID_PARAM; } // Bounds check
        target_lba = fs->root_dir_start_lba + dir_sector_idx_in_chain;
    } else if (fctx->dir_entry_cluster >= 2) { // Subdirectory or FAT32 root directory
        uint32_t current_dir_cluster = fctx->dir_entry_cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = dir_sector_idx_in_chain / sectors_per_cluster;
        uint32_t sector_in_target_cluster = dir_sector_idx_in_chain % sectors_per_cluster;

        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_dir_cluster, &next_cluster);
            if (ret != FS_SUCCESS) { return ret; }
            if (next_cluster >= fs->eoc_marker) { return FS_ERR_CORRUPT; }
            current_dir_cluster = next_cluster;
        }
        target_lba = fat_cluster_to_lba(fs, current_dir_cluster);
        if (target_lba == 0) { return FS_ERR_IO; }
        target_lba += sector_in_target_cluster;
    } else {
        return FS_ERR_INVALID_PARAM;
    }

    // Read-Modify-Write the directory sector via buffer cache
    // serial_printf("[FAT_IO_Update] Modifying directory sector for size at LBA %lu\n", (unsigned long)target_lba);
    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, target_lba);
    if (!b) {
        serial_printf("[FAT_IO_ERR] DirEntry Update: Failed to get buffer for LBA %lu (size update)\n", (unsigned long)target_lba);
        return FS_ERR_IO;
    }

    fat_dir_entry_t* entry_in_buffer = (fat_dir_entry_t*)(b->data + offset_in_dir_sector);
    entry_in_buffer->file_size = fctx->file_size;
    // TODO: Update modification timestamps here as well.

    buffer_mark_dirty(b);
    buffer_release(b);

    // serial_printf("[FAT_IO_Update] DirEntry FileSize successfully updated on disk (via cache) for LBA %lu.\n", (unsigned long)target_lba);
    return FS_SUCCESS;
}