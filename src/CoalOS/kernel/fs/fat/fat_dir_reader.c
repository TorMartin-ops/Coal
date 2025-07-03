/**
 * @file fat_dir_reader.c
 * @brief Directory Reading and Iteration for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles directory reading operations including readdir functionality,
 * LFN reconstruction, and directory entry iteration. Manages directory traversal
 * state and provides dirent structures to VFS layer.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_dir_reader.h"
#include "fat_dir_search.h"
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/fat/fat_dir.h>
#include <kernel/fs/fat/fat_utils.h>
#include <kernel/fs/fat/fat_lfn.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/sync/spinlock.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>

//============================================================================
// Reader Configuration
//============================================================================
// Directory type constants
#ifndef DT_DIR
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

// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[DirReader DEBUG] " fmt "\n", ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[DirReader INFO ] " fmt "\n", ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[DirReader ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Directory Reading Implementation
//============================================================================

int fat_dir_reader_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, 
                                    size_t entry_index)
{
    FAT_DEBUG_LOG("Enter: dir_file=%p, d_entry_out=%p, entry_index=%lu", 
                  dir_file, d_entry_out, (unsigned long)entry_index);

    if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out) {
        FAT_ERROR_LOG("Invalid parameters: dir_file=%p, vnode=%p, data=%p, d_entry_out=%p",
                      dir_file, dir_file ? dir_file->vnode : NULL,
                      dir_file && dir_file->vnode ? dir_file->vnode->data : NULL, d_entry_out);
        return FS_ERR_INVALID_PARAM;
    }

    fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
    if (!fctx->fs || !fctx->is_directory) {
        FAT_ERROR_LOG("Context error: fs=%p, is_directory=%d. Not a valid directory context", 
                     fctx->fs, fctx->is_directory);
        return FS_ERR_NOT_A_DIRECTORY;
    }
    
    fat_fs_t *fs = fctx->fs;
    FAT_DEBUG_LOG("Context valid: fs=%p, first_cluster=%lu", fs, (unsigned long)fctx->first_cluster);

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);

    // State Management
    FAT_DEBUG_LOG("Checking readdir state: requested_idx=%lu, last_idx=%lu, current_cluster=%lu, current_offset=%lu",
                  (unsigned long)entry_index, (unsigned long)fctx->readdir_last_index,
                  (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);

    if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
        // Reset to beginning or seeking backwards
        FAT_DEBUG_LOG("Resetting directory read state");
        int reset_res = fat_dir_reader_reset_state(fctx);
        if (reset_res != FS_SUCCESS) {
            spinlock_release_irqrestore(&fs->lock, irq_flags);
            return reset_res;
        }
    }

    bool is_fat12_16_root = (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0);
    size_t current_logical_index = fctx->readdir_last_index + 1;
    
    // Allocate sector buffer
    uint8_t *sector_buffer = kmalloc(fs->bytes_per_sector);
    if (!sector_buffer) {
        FAT_ERROR_LOG("Failed to allocate sector buffer (%u bytes)", fs->bytes_per_sector);
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        return FS_ERR_OUT_OF_MEMORY;
    }

    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
    int lfn_count = 0;
    int ret = FS_ERR_NOT_FOUND;

    FAT_DEBUG_LOG("Starting directory scan from logical index %lu", (unsigned long)current_logical_index);

    // Main directory scanning loop
    while (true) {
        // Check bounds for different directory types
        if (!is_fat12_16_root && fctx->readdir_current_cluster >= fs->eoc_marker) {
            FAT_DEBUG_LOG("End of directory cluster chain reached");
            ret = FS_ERR_NOT_FOUND;
            break;
        }
        
        if (is_fat12_16_root && 
            fctx->readdir_current_offset >= fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached");
            ret = FS_ERR_NOT_FOUND;
            break;
        }

        // Read current directory sector
        uint32_t sector_idx = fctx->readdir_current_offset / fs->bytes_per_sector;
        int read_res = fat_dir_search_read_sector(fs, fctx->readdir_current_cluster, 
                                                 sector_idx, sector_buffer);
        if (read_res != FS_SUCCESS) {
            FAT_ERROR_LOG("Failed to read directory sector (err %d)", read_res);
            ret = read_res;
            break;
        }

        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
        size_t start_entry = (fctx->readdir_current_offset % fs->bytes_per_sector) / sizeof(fat_dir_entry_t);

        // Process entries in current sector
        for (size_t i = start_entry; i < entries_per_sector; i++) {
            fat_dir_entry_t *dent = (fat_dir_entry_t*)(sector_buffer + i * sizeof(fat_dir_entry_t));
            
            // Update current offset
            fctx->readdir_current_offset = sector_idx * fs->bytes_per_sector + 
                                          (i + 1) * sizeof(fat_dir_entry_t);

            FAT_DEBUG_LOG("Processing entry: offset=%lu, name[0]=0x%02x, attr=0x%02x",
                          (unsigned long)fctx->readdir_current_offset - sizeof(fat_dir_entry_t),
                          dent->name[0], dent->attr);

            // Check for end of directory
            if (dent->name[0] == FAT_DIR_ENTRY_UNUSED) {
                FAT_DEBUG_LOG("Hit end of directory (UNUSED marker)");
                ret = FS_ERR_NOT_FOUND;
                goto readdir_done;
            }

            // Skip deleted entries
            if (dent->name[0] == FAT_DIR_ENTRY_DELETED || dent->name[0] == FAT_DIR_ENTRY_KANJI) {
                lfn_count = 0;
                continue;
            }

            // Skip volume labels (but not LFN entries)
            if ((dent->attr & FAT_ATTR_VOLUME_ID) && !(dent->attr & FAT_ATTR_LONG_NAME)) {
                lfn_count = 0;
                continue;
            }

            // Handle LFN entries
            if ((dent->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                    lfn_collector[lfn_count++] = *lfn_ent;
                    FAT_DEBUG_LOG("Stored LFN entry %d", lfn_count);
                } else {
                    FAT_ERROR_LOG("LFN entry sequence exceeded buffer (%d entries). Discarding LFN", 
                                 FAT_MAX_LFN_ENTRIES);
                    lfn_count = 0;
                }
                continue;
            }

            // Found an 8.3 entry
            FAT_DEBUG_LOG("Found 8.3 entry: Name='%.11s', Attr=0x%02x", dent->name, dent->attr);

            if (current_logical_index == entry_index) {
                FAT_INFO_LOG("Target logical index %lu found!", (unsigned long)entry_index);
                
                // Reconstruct filename
                char final_name[FAT_MAX_LFN_CHARS];
                final_name[0] = '\0';
                
                if (lfn_count > 0) {
                    FAT_DEBUG_LOG("Attempting to reconstruct LFN from %d collected entries", lfn_count);
                    uint8_t expected_sum = fat_calculate_lfn_checksum(dent->name);
                    if (lfn_collector[0].checksum == expected_sum) {
                        fat_reconstruct_lfn(lfn_collector, lfn_count, final_name, sizeof(final_name));
                        if (final_name[0] != '\0') { 
                            FAT_DEBUG_LOG("LFN reconstruction successful: '%s'", final_name); 
                        } else { 
                            FAT_ERROR_LOG("LFN reconstruction failed. Using 8.3 name"); 
                        }
                    } else {
                        FAT_ERROR_LOG("LFN checksum mismatch. Discarding LFN");
                        lfn_count = 0;
                    }
                } else { 
                    FAT_DEBUG_LOG("No preceding LFN entries found"); 
                }
                
                if (final_name[0] == '\0') {
                    fat_dir_reader_format_short_name(dent->name, final_name);
                    FAT_DEBUG_LOG("Using formatted 8.3 name: '%s'", final_name);
                }

                // Populate output dirent
                FAT_DEBUG_LOG("Populating output dirent: name='%s', cluster=%lu, attr=0x%02x",
                              final_name, (unsigned long)fat_get_entry_cluster(dent), dent->attr);
                
                strncpy(d_entry_out->d_name, final_name, sizeof(d_entry_out->d_name) - 1);
                d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0';
                d_entry_out->d_ino = fat_get_entry_cluster(dent);
                d_entry_out->d_type = (dent->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;

                // Update state
                fctx->readdir_last_index = entry_index;
                FAT_DEBUG_LOG("Updated context state: last_index=%lu, current_cluster=%lu, current_offset=%lu",
                              (unsigned long)fctx->readdir_last_index, 
                              (unsigned long)fctx->readdir_current_cluster,
                              (unsigned long)fctx->readdir_current_offset);
                ret = FS_SUCCESS;
                goto readdir_done;
            }

            // Not the target entry
            FAT_DEBUG_LOG("Logical index %lu does not match target %lu. Incrementing logical index",
                          (unsigned long)current_logical_index, (unsigned long)entry_index);
            current_logical_index++;
            lfn_count = 0;
        } // End loop through entries in sector

        // Move to next sector/cluster
        if (!is_fat12_16_root && (fctx->readdir_current_offset % fs->cluster_size_bytes == 0) && 
            fctx->readdir_current_offset > 0) {
            FAT_DEBUG_LOG("End of cluster %lu reached (offset %lu). Finding next cluster",
                          (unsigned long)fctx->readdir_current_cluster, 
                          (unsigned long)fctx->readdir_current_offset);
            
            int advance_res = fat_dir_reader_advance_cluster(fs, fctx);
            if (advance_res != FS_SUCCESS) {
                ret = advance_res;
                break;
            }
        }
    } // End while(true) loop

readdir_done:
    FAT_DEBUG_LOG("Exiting: Releasing lock, freeing buffer %p, returning status %d (%s)",
                   sector_buffer, ret, fs_strerror(ret));
    kfree(sector_buffer);
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    return ret;
}

//============================================================================
// Helper Function Implementations
//============================================================================

int fat_dir_reader_reset_state(fat_file_context_t *file_ctx)
{
    KERNEL_ASSERT(file_ctx && file_ctx->fs, "Invalid file context");
    
    fat_fs_t *fs = file_ctx->fs;
    
    if (fs->type == FAT_TYPE_FAT32 || file_ctx->first_cluster != 0) {
        file_ctx->readdir_current_cluster = file_ctx->first_cluster;
    } else {
        // FAT12/16 root directory
        file_ctx->readdir_current_cluster = 0;
    }
    
    file_ctx->readdir_current_offset = 0;
    file_ctx->readdir_last_index = (size_t)-1;
    
    FAT_DEBUG_LOG("Reset directory state: cluster=%lu, offset=0", 
                  (unsigned long)file_ctx->readdir_current_cluster);
    return FS_SUCCESS;
}

void fat_dir_reader_format_short_name(const uint8_t name_8_3[11], char *out_name)
{
    memcpy(out_name, name_8_3, 8);
    int base_len = 8;
    while (base_len > 0 && out_name[base_len - 1] == ' ') base_len--;
    out_name[base_len] = '\0';
    
    if (name_8_3[8] != ' ') {
        out_name[base_len] = '.';
        base_len++;
        memcpy(out_name + base_len, name_8_3 + 8, 3);
        int ext_len = 3;
        while (ext_len > 0 && out_name[base_len + ext_len - 1] == ' ') ext_len--;
        out_name[base_len + ext_len] = '\0';
    }
}

int fat_dir_reader_advance_cluster(fat_fs_t *fs, fat_file_context_t *file_ctx)
{
    KERNEL_ASSERT(fs && file_ctx, "Invalid parameters");
    
    uint32_t next_cluster;
    int get_next_res = fat_get_next_cluster(fs, file_ctx->readdir_current_cluster, &next_cluster);
    if (get_next_res != FS_SUCCESS) {
        FAT_ERROR_LOG("Failed to get next cluster after %lu (err %d)", 
                     (unsigned long)file_ctx->readdir_current_cluster, get_next_res);
        return get_next_res;
    }
    
    FAT_DEBUG_LOG("Next cluster in chain is %lu", (unsigned long)next_cluster);
    if (next_cluster >= fs->eoc_marker) {
        FAT_DEBUG_LOG("Reached end of cluster chain");
        return FS_ERR_NOT_FOUND;
    }
    
    file_ctx->readdir_current_cluster = next_cluster;
    file_ctx->readdir_current_offset = 0;
    FAT_DEBUG_LOG("Moved to next cluster: cluster=%lu, offset=0", 
                  (unsigned long)file_ctx->readdir_current_cluster);
    
    return FS_SUCCESS;
}