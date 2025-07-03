/**
 * @file fat_path_resolver.c
 * @brief Path Resolution and Component Navigation for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles path parsing, component traversal, and directory navigation.
 * Focuses purely on resolving filesystem paths to directory entries without
 * performing file operations.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_path_resolver.h"
#include "fat_dir_search.h"
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/fat/fat_dir.h>
#include <kernel/fs/fat/fat_utils.h>
#include <kernel/fs/vfs/fs_util.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>

//============================================================================
// Path Resolution Configuration
//============================================================================
// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[PathResolver DEBUG] " fmt "\n", ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[PathResolver INFO ] " fmt "\n", ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[PathResolver ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

//============================================================================
// Path Resolution Implementation
//============================================================================

int fat_path_resolver_lookup(fat_fs_t *fs, const char *path,
                            fat_dir_entry_t *entry_out,
                            char *lfn_out, size_t lfn_max_len,
                            uint32_t *entry_dir_cluster_out,
                            uint32_t *entry_offset_in_dir_out)
{
    KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && 
                  entry_dir_cluster_out != NULL && entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_path_resolver_lookup for required arguments");

    FAT_DEBUG_LOG("Resolving path: '%s'", path);

    // Handle root directory case
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        FAT_DEBUG_LOG("Handling as root directory");
        return fat_path_resolver_get_root_entry(fs, entry_out, lfn_out, lfn_max_len,
                                               entry_dir_cluster_out, entry_offset_in_dir_out);
    }

    // Parse path into components
    char *path_copy = kmalloc(strlen(path) + 1);
    if (!path_copy) {
        FAT_ERROR_LOG("Failed to allocate memory for path copy");
        return FS_ERR_OUT_OF_MEMORY;
    }
    strcpy(path_copy, path);

    char *component = strtok(path_copy, "/");
    uint32_t current_dir_cluster = (fs->type == FAT_TYPE_FAT32) ? fs->root_cluster : 0;
    fat_dir_entry_t current_entry;
    memset(&current_entry, 0, sizeof(current_entry));
    current_entry.attr = FAT_ATTR_DIRECTORY;
    uint32_t previous_dir_cluster = 0;
    int ret = FS_ERR_NOT_FOUND;

    FAT_DEBUG_LOG("Starting traversal from root cluster %lu", (unsigned long)current_dir_cluster);

    // Traverse path components
    while (component != NULL) {
        FAT_DEBUG_LOG("Processing component: '%s' in cluster %lu", component, (unsigned long)current_dir_cluster);
        
        // Handle special directory entries
        if (strcmp(component, ".") == 0) {
            component = strtok(NULL, "/");
            continue;
        }
        
        if (strcmp(component, "..") == 0) {
            FAT_ERROR_LOG("'..' traversal not supported");
            ret = FS_ERR_NOT_SUPPORTED;
            goto lookup_done;
        }

        previous_dir_cluster = current_dir_cluster;
        uint32_t component_entry_offset;
        
        // Find component in current directory
        int find_comp_res = fat_dir_search_find_in_dir(fs, current_dir_cluster, component,
                                                      &current_entry, lfn_out, lfn_max_len,
                                                      &component_entry_offset, NULL);
        if (find_comp_res != FS_SUCCESS) {
            FAT_DEBUG_LOG("Component '%s' not found in cluster %lu (error %d)", 
                         component, (unsigned long)current_dir_cluster, find_comp_res);
            ret = find_comp_res;
            goto lookup_done;
        }

        FAT_DEBUG_LOG("Found component '%s': cluster=%lu, attr=0x%02x", 
                     component, (unsigned long)fat_get_entry_cluster(&current_entry), current_entry.attr);

        // Check if this is the final component
        char* next_component = strtok(NULL, "/");
        if (next_component == NULL) {
            // Found final component - return its information
            memcpy(entry_out, &current_entry, sizeof(*entry_out));
            *entry_dir_cluster_out = previous_dir_cluster;
            *entry_offset_in_dir_out = component_entry_offset;
            ret = FS_SUCCESS;
            FAT_DEBUG_LOG("Path resolution successful: final entry at cluster=%lu, offset=%lu",
                         (unsigned long)previous_dir_cluster, (unsigned long)component_entry_offset);
            goto lookup_done;
        }

        // Not final component - must be a directory to continue
        if (!(current_entry.attr & FAT_ATTR_DIRECTORY)) {
            FAT_ERROR_LOG("Component '%s' is not a directory, cannot continue path traversal", component);
            ret = FS_ERR_NOT_A_DIRECTORY;
            goto lookup_done;
        }

        // Move to next directory
        current_dir_cluster = fat_get_entry_cluster(&current_entry);
        if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0 && previous_dir_cluster != 0) {
            FAT_ERROR_LOG("Invalid cluster 0 for non-root directory in FAT12/16");
            ret = FS_ERR_INVALID_FORMAT;
            goto lookup_done;
        }
        
        component = next_component;
    }

lookup_done:
    FAT_DEBUG_LOG("Path resolution complete: path='%s', result=%d (%s)", 
                 path, ret, fs_strerror(ret));
    kfree(path_copy);
    return ret;
}

int fat_path_resolver_get_root_entry(fat_fs_t *fs, fat_dir_entry_t *entry_out,
                                    char *lfn_out, size_t lfn_max_len,
                                    uint32_t *entry_dir_cluster_out,
                                    uint32_t *entry_offset_in_dir_out)
{
    KERNEL_ASSERT(fs && entry_out && entry_dir_cluster_out && entry_offset_in_dir_out,
                  "Invalid parameters to get_root_entry");

    FAT_DEBUG_LOG("Creating root directory entry for FAT%s", 
                 (fs->type == FAT_TYPE_FAT32) ? "32" : "12/16");

    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->attr = FAT_ATTR_DIRECTORY;
    *entry_offset_in_dir_out = 0;

    if (fs->type == FAT_TYPE_FAT32) {
        entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
        entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
        *entry_dir_cluster_out = 0;
        FAT_DEBUG_LOG("FAT32 root: cluster=%lu", (unsigned long)fs->root_cluster);
    } else {
        entry_out->first_cluster_low  = 0;
        entry_out->first_cluster_high = 0;
        *entry_dir_cluster_out = 0;
        FAT_DEBUG_LOG("FAT12/16 root: fixed location");
    }

    if (lfn_out && lfn_max_len > 0) { 
        strncpy(lfn_out, "/", lfn_max_len - 1); 
        lfn_out[lfn_max_len - 1] = '\0'; 
    }

    return FS_SUCCESS;
}

int fat_path_resolver_split_path(const char *full_path, char *parent_path, size_t parent_max,
                                char *component_name, size_t component_max)
{
    KERNEL_ASSERT(full_path && parent_path && component_name, 
                  "Invalid parameters to split_path");

    FAT_DEBUG_LOG("Splitting path: '%s'", full_path);

    // Use VFS utility for path splitting
    int result = fs_util_split_path(full_path, parent_path, parent_max, 
                                   component_name, component_max);
    
    if (result != 0) {
        FAT_ERROR_LOG("Path splitting failed for '%s'", full_path);
        return FS_ERR_NAMETOOLONG;
    }

    // Validate component name
    if (strlen(component_name) == 0 || 
        strcmp(component_name, ".") == 0 || 
        strcmp(component_name, "..") == 0) {
        FAT_ERROR_LOG("Invalid component name: '%s'", component_name);
        return FS_ERR_INVALID_PARAM;
    }

    FAT_DEBUG_LOG("Path split successful: parent='%s', component='%s'", 
                 parent_path, component_name);
    return FS_SUCCESS;
}

bool fat_path_resolver_is_root_path(const char *path)
{
    return (path && (path[0] == '\0' || strcmp(path, "/") == 0));
}

int fat_path_resolver_validate_component(const char *component)
{
    if (!component || strlen(component) == 0) {
        return FS_ERR_INVALID_PARAM;
    }

    if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
        return FS_ERR_INVALID_PARAM;
    }

    // Additional validation can be added here (length limits, character validation, etc.)
    
    return FS_SUCCESS;
}