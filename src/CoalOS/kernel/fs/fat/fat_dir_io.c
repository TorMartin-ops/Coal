/**
 * @file fat_dir_io.c
 * @brief Low-Level Directory I/O Operations for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles low-level directory sector I/O operations including reading,
 * writing, and updating directory entries. Manages buffer cache interactions
 * and provides sector-level directory access primitives.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_dir_io.h"
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/fat/fat_utils.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/drivers/storage/buffer_cache.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>

//============================================================================
// I/O Configuration
//============================================================================
// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[DirIO DEBUG] " fmt "\n", ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[DirIO INFO ] " fmt "\n", ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[DirIO ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Directory I/O Implementation
//============================================================================

int fat_dir_io_update_entry(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const fat_dir_entry_t *new_entry)
{
    KERNEL_ASSERT(fs != NULL && new_entry != NULL, 
                  "FS context and new entry cannot be NULL in update_directory_entry");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t sector_size = fs->bytes_per_sector;
    uint32_t sector_offset_in_chain = dir_offset / sector_size;
    size_t offset_in_sector = dir_offset % sector_size;

    KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, 
                  "Directory entry offset misaligned");
    KERNEL_ASSERT(offset_in_sector + sizeof(fat_dir_entry_t) <= sector_size, 
                  "Directory entry update crosses sector boundary");

    uint32_t lba;
    int ret = fat_dir_io_calculate_lba(fs, dir_cluster, sector_offset_in_chain, &lba);
    if (ret != FS_SUCCESS) {
        FAT_ERROR_LOG("Failed to calculate LBA for dir_cluster=%lu, sector_offset=%lu", 
                     (unsigned long)dir_cluster, (unsigned long)sector_offset_in_chain);
        return ret;
    }

    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) {
        FAT_ERROR_LOG("Failed to get buffer for LBA %lu", (unsigned long)lba);
        return FS_ERR_IO;
    }
    
    memcpy(b->data + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
    buffer_mark_dirty(b);
    buffer_release(b);
    
    FAT_DEBUG_LOG("Updated directory entry at cluster=%lu, offset=%lu (LBA %lu)",
                  (unsigned long)dir_cluster, (unsigned long)dir_offset, (unsigned long)lba);
    return FS_SUCCESS;
}

int fat_dir_io_mark_entries_deleted(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t first_entry_offset,
                                   size_t num_entries,
                                   uint8_t marker)
{
    KERNEL_ASSERT(fs != NULL && num_entries > 0, 
                  "FS context must be valid and num_entries > 0");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t sector_size = fs->bytes_per_sector;
    int result = FS_SUCCESS;
    size_t entries_marked = 0;
    uint32_t current_offset = first_entry_offset;

    FAT_DEBUG_LOG("Marking %lu entries as deleted starting at offset %lu with marker 0x%02x",
                  (unsigned long)num_entries, (unsigned long)first_entry_offset, marker);

    while (entries_marked < num_entries) {
        uint32_t sector_offset_in_chain = current_offset / sector_size;
        size_t offset_in_sector = current_offset % sector_size;
        KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, 
                      "Entry offset misaligned in mark");

        uint32_t lba;
        result = fat_dir_io_calculate_lba(fs, dir_cluster, sector_offset_in_chain, &lba);
        if (result != FS_SUCCESS) {
            FAT_ERROR_LOG("Failed to calculate LBA for sector %lu in cluster %lu", 
                         (unsigned long)sector_offset_in_chain, (unsigned long)dir_cluster);
            break;
        }

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
        if (!b) {
            FAT_ERROR_LOG("Failed to get buffer for LBA %lu", (unsigned long)lba);
            result = FS_ERR_IO;
            break;
        }

        bool buffer_dirtied = false;
        while (entries_marked < num_entries && offset_in_sector < sector_size) {
            fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(b->data + offset_in_sector);
            entry_ptr->name[0] = marker;
            buffer_dirtied = true;
            offset_in_sector += sizeof(fat_dir_entry_t);
            current_offset   += sizeof(fat_dir_entry_t);
            entries_marked++;
            
            FAT_DEBUG_LOG("Marked entry %lu at offset %lu", 
                         (unsigned long)entries_marked, (unsigned long)current_offset - sizeof(fat_dir_entry_t));
        }
        
        if (buffer_dirtied) buffer_mark_dirty(b);
        buffer_release(b);
        
        if (result != FS_SUCCESS) break;
    }

    if (result == FS_SUCCESS) {
        FAT_INFO_LOG("Successfully marked %lu directory entries as deleted", 
                     (unsigned long)entries_marked);
    } else {
        FAT_ERROR_LOG("Failed to mark directory entries deleted (marked %lu of %lu, error %d)",
                     (unsigned long)entries_marked, (unsigned long)num_entries, result);
    }

    return result;
}

int fat_dir_io_write_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries)
{
    KERNEL_ASSERT(fs != NULL && entries_buf != NULL, 
                  "FS context and entry buffer cannot be NULL in write_directory_entries");
    if (num_entries == 0) return FS_SUCCESS;
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
    size_t sector_size = fs->bytes_per_sector;
    const uint8_t *src_buf = (const uint8_t *)entries_buf;
    size_t bytes_written = 0;
    int result = FS_SUCCESS;

    FAT_DEBUG_LOG("Writing %lu directory entries (%lu bytes) to cluster=%lu, offset=%lu",
                  (unsigned long)num_entries, (unsigned long)total_bytes,
                  (unsigned long)dir_cluster, (unsigned long)dir_offset);

    while (bytes_written < total_bytes) {
        uint32_t current_abs_offset = dir_offset + (uint32_t)bytes_written;
        uint32_t sector_offset_in_chain = current_abs_offset / sector_size;
        size_t offset_in_sector = current_abs_offset % sector_size;
        KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, 
                      "Write offset misaligned");

        uint32_t lba;
        result = fat_dir_io_calculate_lba(fs, dir_cluster, sector_offset_in_chain, &lba);
        if (result != FS_SUCCESS) {
            FAT_ERROR_LOG("Failed to calculate LBA for sector %lu in cluster %lu", 
                         (unsigned long)sector_offset_in_chain, (unsigned long)dir_cluster);
            break;
        }

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
        if (!b) {
            FAT_ERROR_LOG("Failed to get buffer for LBA %lu", (unsigned long)lba);
            result = FS_ERR_IO;
            break;
        }

        size_t bytes_to_write_this_sector = sector_size - offset_in_sector;
        size_t bytes_remaining_total = total_bytes - bytes_written;
        if (bytes_to_write_this_sector > bytes_remaining_total) {
            bytes_to_write_this_sector = bytes_remaining_total;
        }
        KERNEL_ASSERT(bytes_to_write_this_sector > 0, "Zero bytes to write calculation error");

        memcpy(b->data + offset_in_sector, src_buf + bytes_written, bytes_to_write_this_sector);
        buffer_mark_dirty(b);
        buffer_release(b);
        bytes_written += bytes_to_write_this_sector;
        
        FAT_DEBUG_LOG("Wrote %lu bytes to LBA %lu (total written: %lu/%lu)",
                      (unsigned long)bytes_to_write_this_sector, (unsigned long)lba,
                      (unsigned long)bytes_written, (unsigned long)total_bytes);
    }

    if (result == FS_SUCCESS) {
        FAT_INFO_LOG("Successfully wrote %lu directory entries", (unsigned long)num_entries);
    } else {
        FAT_ERROR_LOG("Failed to write directory entries (wrote %lu of %lu bytes, error %d)",
                     (unsigned long)bytes_written, (unsigned long)total_bytes, result);
    }

    return result;
}

//============================================================================
// Helper Function Implementations
//============================================================================

int fat_dir_io_calculate_lba(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t sector_offset_in_chain,
                             uint32_t *lba_out)
{
    KERNEL_ASSERT(fs && lba_out, "Invalid parameters to calculate_lba");

    if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
        // FAT12/16 root directory
        KERNEL_ASSERT(fs->root_dir_sectors > 0, "FAT12/16 root dir sector count is zero");
        if (sector_offset_in_chain >= fs->root_dir_sectors) {
            FAT_ERROR_LOG("Sector offset %lu exceeds root directory size %u",
                         (unsigned long)sector_offset_in_chain, fs->root_dir_sectors);
            return FS_ERR_INVALID_PARAM;
        }
        *lba_out = fs->root_dir_start_lba + sector_offset_in_chain;
        
    } else if (dir_cluster >= 2) {
        // Regular directory in data area
        KERNEL_ASSERT(fs->sectors_per_cluster > 0, "Invalid sectors_per_cluster in FS context");
        uint32_t current_data_cluster = dir_cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
        uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;

        // Traverse cluster chain if needed
        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            int result = fat_get_next_cluster(fs, current_data_cluster, &next_cluster);
            if (result != FS_SUCCESS) {
                FAT_ERROR_LOG("Failed to traverse cluster chain at cluster %lu", 
                             (unsigned long)current_data_cluster);
                return result;
            }
            if (next_cluster >= fs->eoc_marker) {
                FAT_ERROR_LOG("Unexpected end of cluster chain at cluster %lu", 
                             (unsigned long)current_data_cluster);
                return FS_ERR_INVALID_PARAM;
            }
            current_data_cluster = next_cluster;
        }
        
        uint32_t cluster_lba = fat_cluster_to_lba(fs, current_data_cluster);
        if (cluster_lba == 0) {
            FAT_ERROR_LOG("Failed to convert cluster %lu to LBA", 
                         (unsigned long)current_data_cluster);
            return FS_ERR_IO;
        }
        *lba_out = cluster_lba + sector_in_final_cluster;
        
    } else {
        FAT_ERROR_LOG("Invalid directory cluster %lu", (unsigned long)dir_cluster);
        return FS_ERR_INVALID_PARAM;
    }

    FAT_DEBUG_LOG("Calculated LBA %lu for cluster=%lu, sector_offset=%lu",
                  (unsigned long)*lba_out, (unsigned long)dir_cluster, 
                  (unsigned long)sector_offset_in_chain);
    return FS_SUCCESS;
}

buffer_t* fat_dir_io_get_sector_buffer(fat_fs_t *fs, uint32_t dir_cluster,
                                       uint32_t sector_offset_in_chain,
                                       uint32_t *lba_out)
{
    KERNEL_ASSERT(fs && lba_out, "Invalid parameters to get_sector_buffer");

    uint32_t lba;
    int result = fat_dir_io_calculate_lba(fs, dir_cluster, sector_offset_in_chain, &lba);
    if (result != FS_SUCCESS) {
        FAT_ERROR_LOG("Failed to calculate LBA (error %d)", result);
        return NULL;
    }

    buffer_t* buffer = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!buffer) {
        FAT_ERROR_LOG("Failed to get buffer for LBA %lu", (unsigned long)lba);
        return NULL;
    }

    *lba_out = lba;
    FAT_DEBUG_LOG("Got buffer for LBA %lu (cluster=%lu, sector_offset=%lu)",
                  (unsigned long)lba, (unsigned long)dir_cluster, 
                  (unsigned long)sector_offset_in_chain);
    return buffer;
}