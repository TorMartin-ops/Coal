/**
 * @file fat_dir_main.c
 * @brief Main Directory Operations Coordination for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Serves as the main coordination point for all directory-related
 * operations in the FAT filesystem. Provides the public API and delegates
 * to specialized modules following the Facade pattern.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_dir_main.h"
#include "fat_path_resolver.h"
#include "fat_dir_search.h"
#include "fat_dir_operations.h"
#include "fat_dir_reader.h"
#include "fat_dir_io.h"
#include <kernel/drivers/display/serial.h>

//============================================================================
// Main Coordination Configuration
//============================================================================
// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[fat_dir:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[fat_dir:INFO]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[fat_dir:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Public API Implementation - Facade Pattern
//============================================================================

vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_operations_open_internal");
    return fat_dir_operations_open_internal(fs_context, path, flags);
}

int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_reader_readdir_internal");
    return fat_dir_reader_readdir_internal(dir_file, d_entry_out, entry_index);
}

int fat_unlink_internal(void *fs_context, const char *path)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_operations_unlink_internal");
    return fat_dir_operations_unlink_internal(fs_context, path);
}

int fat_find_in_dir(fat_fs_t *fs,
                   uint32_t dir_cluster,
                   const char *component,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_offset_in_dir_out,
                   uint32_t *first_lfn_offset_out)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_search_find_in_dir");
    return fat_dir_search_find_in_dir(fs, dir_cluster, component, entry_out, 
                                     lfn_out, lfn_max_len, entry_offset_in_dir_out, 
                                     first_lfn_offset_out);
}

int fat_lookup_path(fat_fs_t *fs, const char *path,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_dir_cluster_out,
                   uint32_t *entry_offset_in_dir_out)
{
    FAT_DEBUG_LOG("Delegating to fat_path_resolver_lookup");
    return fat_path_resolver_lookup(fs, path, entry_out, lfn_out, lfn_max_len,
                                   entry_dir_cluster_out, entry_offset_in_dir_out);
}

//============================================================================
// Legacy Support Functions (for compatibility)
//============================================================================

int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                          uint32_t sector_offset_in_chain,
                          uint8_t* buffer)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_search_read_sector (legacy compatibility)");
    return fat_dir_search_read_sector(fs, cluster, sector_offset_in_chain, buffer);
}

int update_directory_entry(fat_fs_t *fs,
                           uint32_t dir_cluster,
                           uint32_t dir_offset,
                           const fat_dir_entry_t *new_entry)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_io_update_entry (legacy compatibility)");
    return fat_dir_io_update_entry(fs, dir_cluster, dir_offset, new_entry);
}

int mark_directory_entries_deleted(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t first_entry_offset,
                                   size_t num_entries,
                                   uint8_t marker)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_io_mark_entries_deleted (legacy compatibility)");
    return fat_dir_io_mark_entries_deleted(fs, dir_cluster, first_entry_offset, 
                                          num_entries, marker);
}

int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const void *entries_buf,
                            size_t num_entries)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_io_write_entries (legacy compatibility)");
    return fat_dir_io_write_entries(fs, dir_cluster, dir_offset, entries_buf, num_entries);
}

int find_free_directory_slot(fat_fs_t *fs,
                             uint32_t parent_dir_cluster,
                             size_t needed_slots,
                             uint32_t *out_slot_cluster,
                             uint32_t *out_slot_offset)
{
    FAT_DEBUG_LOG("Delegating to fat_dir_search_find_free_slots (legacy compatibility)");
    return fat_dir_search_find_free_slots(fs, parent_dir_cluster, needed_slots, 
                                         out_slot_cluster, out_slot_offset);
}