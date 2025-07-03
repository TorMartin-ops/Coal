/**
 * @file fat_dir_search.c
 * @brief Directory Entry Search and LFN Matching for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles searching for specific directory entries within directory
 * clusters, including Long File Name (LFN) sequence matching and reconstruction.
 * Focuses purely on search algorithms without path resolution.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_dir_search.h"
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/fat/fat_dir.h>
#include <kernel/fs/fat/fat_utils.h>
#include <kernel/fs/fat/fat_lfn.h>
#include <kernel/fs/fat/fat_alloc.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/drivers/storage/buffer_cache.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>

//============================================================================
// Search Configuration
//============================================================================
// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[DirSearch DEBUG] " fmt "\n", ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[DirSearch INFO ] " fmt "\n", ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[DirSearch ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Directory Search Implementation
//============================================================================

int fat_dir_search_find_in_dir(fat_fs_t *fs,
                               uint32_t dir_cluster,
                               const char *component,
                               fat_dir_entry_t *entry_out,
                               char *lfn_out, size_t lfn_max_len,
                               uint32_t *entry_offset_in_dir_out,
                               uint32_t *first_lfn_offset_out)
{
    KERNEL_ASSERT(fs != NULL && component != NULL && entry_out != NULL && 
                  entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_dir_search_find_in_dir for required arguments");
    KERNEL_ASSERT(strlen(component) > 0, "Component name cannot be empty");

    FAT_DEBUG_LOG("Enter: Searching for '%s' in dir_cluster %lu", 
                  component, (unsigned long)dir_cluster);

    uint32_t current_cluster = dir_cluster;
    bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
    uint32_t current_byte_offset = 0;

    if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
    if (first_lfn_offset_out) *first_lfn_offset_out = (uint32_t)-1;

    uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
    if (!sector_data) {
        FAT_ERROR_LOG("Failed to allocate sector buffer (%u bytes)", fs->bytes_per_sector);
        return FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p", sector_data);

    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
    int lfn_count = 0;
    uint32_t current_lfn_start_offset = (uint32_t)-1;
    int ret = FS_ERR_NOT_FOUND;

    while (true) {
        FAT_DEBUG_LOG("Loop: current_cluster=%lu, current_byte_offset=%lu", 
                      (unsigned long)current_cluster, (unsigned long)current_byte_offset);
        
        if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
            FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu)", 
                         (unsigned long)current_cluster, (unsigned long)fs->eoc_marker);
            ret = FS_ERR_NOT_FOUND;
            break;
        }
        
        if (scanning_fixed_root && current_byte_offset >= 
            (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu)",
                         (unsigned long)current_byte_offset, 
                         (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
            ret = FS_ERR_NOT_FOUND;
            break;
        }

        uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);

        FAT_DEBUG_LOG("Reading sector: chain_offset=%lu", (unsigned long)sector_offset_in_chain);
        int read_res = fat_dir_search_read_sector(fs, current_cluster, 
                                                 sector_offset_in_chain, sector_data);
        if (read_res != FS_SUCCESS) {
            FAT_ERROR_LOG("read_directory_sector failed (err %d)", read_res);
            ret = read_res;
            break;
        }
        FAT_DEBUG_LOG("Sector read success. Processing %lu entries...", 
                      (unsigned long)entries_per_sector);

        for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
            fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
            uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));

            FAT_DEBUG_LOG("Entry %lu (abs_offset %lu): Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)e_idx, (unsigned long)entry_abs_offset, 
                          de->name[0], de->attr);

            if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                FAT_DEBUG_LOG("Found UNUSED entry marker (0x00). End of directory.");
                ret = FS_ERR_NOT_FOUND;
                goto find_done;
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
                fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)de;
                if (lfn_count == 0) current_lfn_start_offset = entry_abs_offset;
                if (lfn_count < FAT_MAX_LFN_ENTRIES) lfn_collector[lfn_count++] = *lfn_ent;
                else {lfn_count = 0; current_lfn_start_offset = (uint32_t)-1;}
            } else {
                bool match = false;
                char recon_lfn[FAT_MAX_LFN_CHARS];
                
                if (lfn_count > 0) {
                    uint8_t sum = fat_calculate_lfn_checksum(de->name);
                    if (lfn_collector[0].checksum == sum) {
                        fat_reconstruct_lfn(lfn_collector, lfn_count, recon_lfn, sizeof(recon_lfn));
                        if (fat_compare_lfn(component, recon_lfn) == 0) match = true;
                    } else { 
                        lfn_count = 0; 
                        current_lfn_start_offset = (uint32_t)-1; 
                    }
                }
                
                if (!match) { 
                    if (fat_compare_8_3(component, de->name) == 0) match = true; 
                }

                if (match) {
                    memcpy(entry_out, de, sizeof(fat_dir_entry_t));
                    *entry_offset_in_dir_out = entry_abs_offset;
                    if (first_lfn_offset_out) { 
                        *first_lfn_offset_out = current_lfn_start_offset; 
                    }
                    
                    // Reconstruct LFN for output if requested
                    if (lfn_out && lfn_max_len > 0 && lfn_count > 0) {
                        fat_reconstruct_lfn(lfn_collector, lfn_count, lfn_out, lfn_max_len);
                    }
                    
                    ret = FS_SUCCESS;
                    goto find_done;
                }
                lfn_count = 0; 
                current_lfn_start_offset = (uint32_t)-1;
            }
        } // End entry loop

        current_byte_offset += fs->bytes_per_sector;
        FAT_DEBUG_LOG("Advanced to next sector offset: %lu", (unsigned long)current_byte_offset);

        if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
            FAT_DEBUG_LOG("End of cluster %lu reached. Finding next...", (unsigned long)current_cluster);
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) { ret = get_next_res; break; }
            FAT_DEBUG_LOG("Next cluster in chain: %lu", (unsigned long)next_c);
            if (next_c >= fs->eoc_marker) { ret = FS_ERR_NOT_FOUND; break; }
            current_cluster = next_c;
            current_byte_offset = 0;
        }
    } // End while loop

find_done:
    FAT_DEBUG_LOG("Exit: Freeing buffer %p, returning status %d (%s)",
                  sector_data, ret, fs_strerror(ret));
    kfree(sector_data);
    return ret;
}

int fat_dir_search_find_free_slots(fat_fs_t *fs,
                                   uint32_t parent_dir_cluster,
                                   size_t needed_slots,
                                   uint32_t *out_slot_cluster,
                                   uint32_t *out_slot_offset)
{
    FAT_DEBUG_LOG("Enter: Searching for %lu slots in dir_cluster %lu", 
                  (unsigned long)needed_slots, (unsigned long)parent_dir_cluster);
    KERNEL_ASSERT(fs && needed_slots && out_slot_cluster && out_slot_offset, 
                  "find_free_directory_slot: bad args");

    const bool fixed_root = (fs->type != FAT_TYPE_FAT32 && parent_dir_cluster == 0);
    const uint32_t bytes_per_entry = sizeof(fat_dir_entry_t);
    uint32_t cur_cluster   = parent_dir_cluster;
    uint32_t last_cluster  = parent_dir_cluster;
    uint32_t byte_offset   = 0;
    uint32_t cand_offset   = 0;
    size_t   free_run      = 0;

    // Allocate a buffer for reading directory sectors
    uint8_t *sector_buf = kmalloc(fs->bytes_per_sector);
    if (!sector_buf) {
        FAT_ERROR_LOG("Failed to allocate sector buffer.");
        return FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p (%u bytes).", 
                  sector_buf, fs->bytes_per_sector);

    int status = FS_ERR_NO_SPACE;

    // Scan Directory Cluster Chain
    while (true) {
        // Check for end of cluster chain (only if not scanning fixed root)
        if (cur_cluster >= fs->eoc_marker && !fixed_root) {
            FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu)", 
                         (unsigned long)cur_cluster, (unsigned long)fs->eoc_marker);
            break;
        }

        uint32_t sector_idx   = byte_offset / fs->bytes_per_sector;
        uint32_t entries_per_sector = fs->bytes_per_sector / bytes_per_entry;

        // Check for end of fixed-size root directory (FAT12/16)
        if (fixed_root && byte_offset >= fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu)",
                          (unsigned long)byte_offset, 
                          (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
            break;
        }

        FAT_DEBUG_LOG("Loop: current_cluster=%lu, current_byte_offset=%lu", 
                      (unsigned long)cur_cluster, (unsigned long)byte_offset);

        // Read the current directory sector
        status = fat_dir_search_read_sector(fs, cur_cluster, sector_idx, sector_buf);
        if (status != FS_SUCCESS) {
            FAT_ERROR_LOG("read_directory_sector failed (err %d) for cluster %lu, sector_idx %lu",
                           status, (unsigned long)cur_cluster, (unsigned long)sector_idx);
            goto out;
        }
        FAT_DEBUG_LOG("Sector read success (cluster %lu, sector_idx %lu). Processing %lu entries...",
                       (unsigned long)cur_cluster, (unsigned long)sector_idx, 
                       (unsigned long)entries_per_sector);

        // Scan entries within the current sector
        for (size_t i = 0; i < entries_per_sector; ++i) {
            fat_dir_entry_t *de = (fat_dir_entry_t *)(sector_buf + i * bytes_per_entry);
            uint8_t tag = de->name[0];

            uint32_t current_entry_abs_offset = byte_offset + (uint32_t)i * bytes_per_entry;
            FAT_DEBUG_LOG("  Entry %lu (abs_offset %lu): Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)i, (unsigned long)current_entry_abs_offset, 
                          tag, de->attr);

            if (tag == FAT_DIR_ENTRY_UNUSED || tag == FAT_DIR_ENTRY_DELETED) {
                // Found a free or deleted slot
                if (free_run == 0) {
                    cand_offset = current_entry_abs_offset;
                    FAT_DEBUG_LOG("  Found start of free run at offset %lu", 
                                 (unsigned long)cand_offset);
                }
                ++free_run;
                FAT_DEBUG_LOG("  Incremented free run to %lu (needed %lu)", 
                             (unsigned long)free_run, (unsigned long)needed_slots);

                // Check if we found enough contiguous slots OR hit the end marker
                if (free_run >= needed_slots || tag == FAT_DIR_ENTRY_UNUSED) {
                    FAT_DEBUG_LOG("  Condition met: (free_run %lu >= needed %lu) OR (tag==UNUSED %d)",
                                 (unsigned long)free_run, (unsigned long)needed_slots, 
                                 (tag == FAT_DIR_ENTRY_UNUSED));
                    *out_slot_cluster = cur_cluster;
                    *out_slot_offset  = cand_offset;
                    status = FS_SUCCESS;
                    FAT_INFO_LOG("Found suitable slot(s): Cluster=%lu, Offset=%lu (needed %lu, found run %lu)",
                                 (unsigned long)*out_slot_cluster, (unsigned long)*out_slot_offset,
                                 (unsigned long)needed_slots, (unsigned long)free_run);
                    goto out;
                }
            } else {
                // Encountered an in-use entry, reset the free run count
                if (free_run > 0) {
                    FAT_DEBUG_LOG("  Resetting free run count (was %lu)", (unsigned long)free_run);
                }
                free_run = 0;
            }

            if (tag == FAT_DIR_ENTRY_UNUSED) {
                FAT_DEBUG_LOG("  Exiting search loop after hitting UNUSED marker");
                status = FS_ERR_NO_SPACE;
                goto out;
            }
        } // End loop through entries in sector

        // Advance to the next sector's byte offset
        byte_offset += fs->bytes_per_sector;

        // Move to the next cluster if we've finished the current one (and not in fixed root)
        if (!fixed_root && (byte_offset % fs->cluster_size_bytes == 0)) {
            FAT_DEBUG_LOG("End of cluster %lu reached (offset %lu). Finding next...", 
                         (unsigned long)cur_cluster, (unsigned long)byte_offset);
            last_cluster = cur_cluster;
            uint32_t next;
            status = fat_get_next_cluster(fs, cur_cluster, &next);
            if (status != FS_SUCCESS) {
                FAT_ERROR_LOG("Failed to get next cluster after %lu (err %d)", 
                             (unsigned long)cur_cluster, status);
                goto out;
            }
            FAT_DEBUG_LOG("Next cluster in chain: %lu", (unsigned long)next);
            if (next >= fs->eoc_marker) {
                FAT_DEBUG_LOG("Reached end of chain marker (%lu)", (unsigned long)next);
                break;
            }
            cur_cluster  = next;
            byte_offset  = 0;
            free_run = 0;
            FAT_DEBUG_LOG("Moved to next cluster %lu", (unsigned long)cur_cluster);
        }
    } // End while(true) loop

    // Try to extend the directory by allocating a new cluster (only if not fixed root)
    if (status == FS_ERR_NO_SPACE && !fixed_root) {
        FAT_INFO_LOG("No suitable free slot found. Attempting to extend directory (last cluster: %lu)...", 
                     (unsigned long)last_cluster);
        uint32_t new_clu = fat_allocate_cluster(fs, last_cluster);
        if (new_clu == 0 || new_clu < 2) {
            FAT_ERROR_LOG("Failed to allocate new cluster for directory extension (clu %lu)", 
                         (unsigned long)new_clu);
            status = FS_ERR_NO_SPACE;
            goto out;
        }
        FAT_INFO_LOG("Successfully allocated and linked new cluster %lu for directory", 
                     (unsigned long)new_clu);

        // Zero out the newly allocated cluster
        FAT_DEBUG_LOG("Zeroing out new cluster %lu...", (unsigned long)new_clu);
        memset(sector_buf, 0, fs->bytes_per_sector);
        uint32_t lba = fat_cluster_to_lba(fs, new_clu);
        if (lba == 0) {
            FAT_ERROR_LOG("Failed to convert new cluster %lu to LBA!", (unsigned long)new_clu);
            status = FS_ERR_IO;
            goto out;
        }
        
        for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s) {
            FAT_DEBUG_LOG("Zeroing sector %lu (LBA %lu) of new cluster %lu", 
                         (unsigned long)s, (unsigned long)(lba+s), (unsigned long)new_clu);
            buffer_t *b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba + s);
            if (!b) {
                FAT_ERROR_LOG("Failed to get buffer for LBA %lu during zeroing!", 
                             (unsigned long)(lba+s));
                status = FS_ERR_IO;
                goto out;
            }
            memcpy(b->data, sector_buf, fs->bytes_per_sector);
            buffer_mark_dirty(b);
            buffer_release(b);
        }
        FAT_DEBUG_LOG("New cluster %lu zeroed successfully", (unsigned long)new_clu);

        *out_slot_cluster = new_clu;
        *out_slot_offset  = 0;
        status = FS_SUCCESS;
        FAT_INFO_LOG("Directory extended. Free slot at start of new cluster %lu (offset 0)", 
                     (unsigned long)new_clu);
    }

out:
    FAT_DEBUG_LOG("Exit: Freeing buffer %p, returning status %d (%s)", 
                  sector_buf, status, fs_strerror(status));
    kfree(sector_buf);
    return status;
}

int fat_dir_search_read_sector(fat_fs_t *fs, uint32_t cluster,
                              uint32_t sector_offset_in_chain,
                              uint8_t* buffer)
{
    KERNEL_ASSERT(fs != NULL && buffer != NULL, 
                  "FS context and buffer cannot be NULL in read_directory_sector");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector in FS context");

    uint32_t lba;
    int ret = FS_SUCCESS;

    if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
        KERNEL_ASSERT(fs->root_dir_sectors > 0, "FAT12/16 root dir sector count is zero");
        if (sector_offset_in_chain >= fs->root_dir_sectors) return FS_ERR_NOT_FOUND;
        lba = fs->root_dir_start_lba + sector_offset_in_chain;
    } else if (cluster >= 2) {
        KERNEL_ASSERT(fs->sectors_per_cluster > 0, "Invalid sectors_per_cluster in FS context");
        uint32_t current_scan_cluster = cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
        uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;

        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_scan_cluster, &next_cluster);
            if (ret != FS_SUCCESS) return ret;
            if (next_cluster >= fs->eoc_marker) return FS_ERR_NOT_FOUND;
            current_scan_cluster = next_cluster;
        }
        uint32_t cluster_start_lba = fat_cluster_to_lba(fs, current_scan_cluster);
        if (cluster_start_lba == 0) return FS_ERR_IO;
        lba = cluster_start_lba + sector_in_final_cluster;
    } else {
        return FS_ERR_INVALID_PARAM;
    }

    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) return FS_ERR_IO;
    memcpy(buffer, b->data, fs->bytes_per_sector);
    buffer_release(b);
    return FS_SUCCESS;
}