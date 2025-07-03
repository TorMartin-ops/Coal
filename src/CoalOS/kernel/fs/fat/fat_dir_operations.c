/**
 * @file fat_dir_operations.c
 * @brief File and Directory Operations for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles high-level file operations including opening with O_CREAT/O_TRUNC,
 * unlinking files, and directory management. Coordinates between path resolution,
 * search, and I/O operations.
 */

//============================================================================
// Includes
//============================================================================
#include "fat_dir_operations.h"
#include "fat_path_resolver.h"
#include "fat_dir_search.h"
#include "fat_dir_io.h"
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/fat/fat_dir.h>
#include <kernel/fs/fat/fat_utils.h>
#include <kernel/fs/fat/fat_alloc.h>
#include <kernel/fs/vfs/fs_util.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/fs_config.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/drivers/storage/buffer_cache.h>
#include <kernel/sync/spinlock.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>

//============================================================================
// Operations Configuration
//============================================================================
// Logging Macros
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[DirOps DEBUG] " fmt "\n", ##__VA_ARGS__)
#define FAT_INFO_LOG(fmt, ...)  serial_printf("[DirOps INFO ] " fmt "\n", ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) serial_printf("[DirOps ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

// External declarations
extern vfs_driver_t fat_vfs_driver;

//============================================================================
// File Operations Implementation
//============================================================================

vnode_t *fat_dir_operations_open_internal(void *fs_context, const char *path, int flags)
{
    FAT_DEBUG_LOG("Enter: path='%s', flags=0x%x", path ? path : "<NULL>", flags);

    fat_fs_t *fs = (fat_fs_t *)fs_context;
    if (!fs || !path) {
        FAT_ERROR_LOG("Invalid parameters: fs=%p, path=%p", fs, path);
        return NULL;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
    FAT_DEBUG_LOG("Lock acquired");

    fat_dir_entry_t entry;
    char lfn_buffer[FAT_MAX_LFN_CHARS];
    uint32_t entry_dir_cluster = 0;
    uint32_t entry_offset_in_dir = 0;
    int find_res;
    bool exists = false;
    bool created = false;
    bool truncated = false;
    vnode_t *vnode = NULL;
    int ret_err = FS_SUCCESS;

    // Step 1: Lookup the path using path resolver
    FAT_DEBUG_LOG("Step 1: Looking up path '%s'...", path);
    find_res = fat_path_resolver_lookup(fs, path, &entry, lfn_buffer, sizeof(lfn_buffer),
                                       &entry_dir_cluster, &entry_offset_in_dir);
    FAT_DEBUG_LOG("Lookup finished. find_res = %d (%s)", find_res, fs_strerror(find_res));

    // Step 2: Handle Lookup Result
    FAT_DEBUG_LOG("Step 2: Handling lookup result (%d)...", find_res);
    if (find_res == FS_SUCCESS) {
        FAT_DEBUG_LOG("Branch: File/Directory Exists");
        exists = true;
        ret_err = FS_SUCCESS;
        FAT_DEBUG_LOG("Existing entry found: Attr=0x%02x, Size=%lu, Cluster=%lu, DirClu=%lu, DirOff=%lu",
                      entry.attr, (unsigned long)entry.file_size,
                      (unsigned long)fat_get_entry_cluster(&entry),
                      (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);

        bool is_dir = (entry.attr & FAT_ATTR_DIRECTORY);

        // Check O_EXCL flag if creating
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            FAT_ERROR_LOG("File '%s' exists and O_CREAT|O_EXCL flags were specified", path);
            ret_err = FS_ERR_FILE_EXISTS;
            goto open_fail_locked;
        }
        
        // Check if trying to write/truncate a directory
        if (is_dir && (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND))) {
            FAT_ERROR_LOG("Cannot open directory '%s' with write/truncate/append flags (0x%x)", 
                         path, flags);
            ret_err = FS_ERR_IS_A_DIRECTORY;
            goto open_fail_locked;
        }
        
        // Handle truncation
        if (!is_dir && (flags & O_TRUNC)) {
            if (!(flags & (O_WRONLY | O_RDWR))) {
                FAT_ERROR_LOG("O_TRUNC specified for '%s' but no write permission requested (flags 0x%x)", 
                             path, flags);
                ret_err = FS_ERR_PERMISSION_DENIED;
                goto open_fail_locked;
            }
            FAT_INFO_LOG("Handling O_TRUNC for existing file '%s', original size=%lu", 
                        path, (unsigned long)entry.file_size);
            if (entry.file_size > 0) {
                int trunc_res = fat_dir_operations_truncate_file(fs, &entry, 
                                                               entry_dir_cluster, entry_offset_in_dir);
                if (trunc_res != FS_SUCCESS) {
                    FAT_ERROR_LOG("truncate_file failed for '%s', error: %d (%s)", 
                                 path, trunc_res, fs_strerror(trunc_res));
                    ret_err = trunc_res;
                    goto open_fail_locked;
                }
                FAT_DEBUG_LOG("Truncate successful");
                truncated = true;
            } else {
                FAT_DEBUG_LOG("File already size 0, no truncation needed");
                truncated = true;
            }
        }

    } else if (find_res == FS_ERR_NOT_FOUND) {
        FAT_DEBUG_LOG("Branch: File/Directory Not Found");
        exists = false;
        ret_err = find_res;

        bool should_create = (flags & O_CREAT);
        FAT_DEBUG_LOG("Checking O_CREAT flag: Present=%d", should_create);
        if (should_create) {
            FAT_INFO_LOG("O_CREAT flag set. Attempting file creation for path '%s'...", path);
            int create_res = fat_create_file(fs, path, FAT_ATTR_ARCHIVE,
                                           &entry,
                                           &entry_dir_cluster,
                                           &entry_offset_in_dir);
            FAT_DEBUG_LOG("fat_create_file returned %d", create_res);

            if (create_res == FS_SUCCESS) {
                created = true;
                exists = true;
                ret_err = FS_SUCCESS;
                FAT_DEBUG_LOG("O_CREAT successful, new entry info: Size=%lu, Cluster=%lu, DirClu=%lu, DirOff=%lu",
                              (unsigned long)entry.file_size, (unsigned long)fat_get_entry_cluster(&entry),
                              (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
            } else {
                FAT_ERROR_LOG("fat_create_file failed for '%s', error: %d (%s)", 
                             path, create_res, fs_strerror(create_res));
                ret_err = create_res;
                FAT_DEBUG_LOG("Setting ret_err to %d and jumping to fail path", ret_err);
                goto open_fail_locked;
            }
        } else {
            FAT_DEBUG_LOG("O_CREAT not specified. File not found error (%d) persists", ret_err);
            goto open_fail_locked;
        }
    } else {
        // Other error during lookup
        FAT_DEBUG_LOG("Branch: Other Lookup Error (%d)", find_res);
        FAT_ERROR_LOG("Lookup failed for '%s' with unexpected error: %d (%s)", 
                     path, find_res, fs_strerror(find_res));
        ret_err = find_res;
        goto open_fail_locked;
    }

    // Step 3: Create vnode and context
    FAT_DEBUG_LOG("Step 3: Creating vnode and file context...");
    vnode = fat_dir_operations_create_vnode(fs, &entry, entry_dir_cluster, 
                                           entry_offset_in_dir, created, truncated);
    if (!vnode) {
        FAT_ERROR_LOG("Failed to create vnode for '%s'", path);
        ret_err = FS_ERR_OUT_OF_MEMORY;
        goto open_fail_locked;
    }

    // Success
    FAT_DEBUG_LOG("Step 4: Success Path");
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    FAT_DEBUG_LOG("Lock released");
    FAT_INFO_LOG("Open successful: path='%s', vnode=%p", path, vnode);
    return vnode;

open_fail_locked:
    FAT_DEBUG_LOG("Step F: Failure Path Entered (ret_err=%d)", ret_err);
    FAT_ERROR_LOG("Open failed: path='%s', error=%d (%s)", path, ret_err, fs_strerror(ret_err));
    if (vnode) { 
        FAT_DEBUG_LOG("Freeing vnode %p", vnode); 
        kfree(vnode); 
    }
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    FAT_DEBUG_LOG("Lock released");
    return NULL;
}

int fat_dir_operations_unlink_internal(void *fs_context, const char *path)
{
    fat_fs_t *fs = (fat_fs_t*)fs_context;
    if (!fs || !path) return FS_ERR_INVALID_PARAM;

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
    int ret = FS_SUCCESS;

    // Split path into parent directory path and final component name
    char parent_path[FS_MAX_PATH_LENGTH];
    char component_name[MAX_FILENAME_LEN + 1];
    if (fat_path_resolver_split_path(path, parent_path, sizeof(parent_path), 
                                    component_name, sizeof(component_name)) != FS_SUCCESS) {
        ret = FS_ERR_NAMETOOLONG;
        goto unlink_fail_locked;
    }
    
    if (strlen(component_name) == 0 || strcmp(component_name, ".") == 0 || 
        strcmp(component_name, "..") == 0) {
        ret = FS_ERR_INVALID_PARAM;
        goto unlink_fail_locked;
    }

    // Lookup the parent directory
    fat_dir_entry_t parent_entry;
    uint32_t parent_entry_dir_cluster, parent_entry_offset;
    int parent_res = fat_path_resolver_lookup(fs, parent_path, &parent_entry, NULL, 0,
                                             &parent_entry_dir_cluster, &parent_entry_offset);
    if (parent_res != FS_SUCCESS) {
        ret = parent_res;
        goto unlink_fail_locked;
    }
    
    if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
        ret = FS_ERR_NOT_A_DIRECTORY;
        goto unlink_fail_locked;
    }
    
    uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
    if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_path, "/") == 0) {
        parent_cluster = 0;
    }

    // Find the actual entry within the parent directory
    fat_dir_entry_t entry_to_delete;
    uint32_t entry_offset;
    uint32_t first_lfn_offset = (uint32_t)-1;
    int find_res = fat_dir_search_find_in_dir(fs, parent_cluster, component_name,
                                              &entry_to_delete, NULL, 0,
                                              &entry_offset, &first_lfn_offset);

    if (find_res != FS_SUCCESS) {
        ret = find_res;
        goto unlink_fail_locked;
    }

    // Perform checks
    if (entry_to_delete.attr & FAT_ATTR_DIRECTORY) {
        ret = FS_ERR_IS_A_DIRECTORY;
        goto unlink_fail_locked;
    }
    
    if (entry_to_delete.attr & FAT_ATTR_READ_ONLY) {
        ret = FS_ERR_PERMISSION_DENIED;
        goto unlink_fail_locked;
    }

    // Free cluster chain
    uint32_t file_cluster = fat_get_entry_cluster(&entry_to_delete);
    if (file_cluster >= 2) {
        int free_res = fat_free_cluster_chain(fs, file_cluster);
        if (free_res != FS_SUCCESS) {
            FAT_ERROR_LOG("Warning: Error freeing cluster chain for '%s' (err %d)", path, free_res);
            ret = free_res;
        }
    }

    // Mark directory entries as deleted
    size_t num_entries_to_mark = 1;
    uint32_t mark_start_offset = entry_offset;

    if (first_lfn_offset != (uint32_t)-1 && first_lfn_offset < entry_offset) {
        num_entries_to_mark = ((entry_offset - first_lfn_offset) / sizeof(fat_dir_entry_t)) + 1;
        mark_start_offset = first_lfn_offset;
    }

    int mark_res = fat_dir_io_mark_entries_deleted(fs, parent_cluster,
                                                  mark_start_offset,
                                                  num_entries_to_mark,
                                                  FAT_DIR_ENTRY_DELETED);
    if (mark_res != FS_SUCCESS) {
        FAT_ERROR_LOG("Error marking directory entry deleted for '%s' (err %d)", path, mark_res);
        ret = mark_res;
        goto unlink_fail_locked;
    }

    // Flush changes
    buffer_cache_sync();

    FAT_INFO_LOG("Successfully unlinked '%s'", path);

unlink_fail_locked:
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    return ret;
}

int fat_dir_operations_truncate_file(fat_fs_t *fs, fat_dir_entry_t *entry,
                                     uint32_t entry_dir_cluster, uint32_t entry_offset)
{
    KERNEL_ASSERT(fs && entry, "Invalid parameters to truncate_file");

    uint32_t first_cluster = fat_get_entry_cluster(entry);
    
    // Free the cluster chain if file has allocated clusters
    if (first_cluster >= 2) {
        int free_res = fat_free_cluster_chain(fs, first_cluster);
        if (free_res != FS_SUCCESS) {
            FAT_ERROR_LOG("Failed to free cluster chain starting at %lu", 
                         (unsigned long)first_cluster);
            return free_res;
        }
    }

    // Update directory entry
    entry->file_size = 0;
    entry->first_cluster_low = 0;
    entry->first_cluster_high = 0;

    // Write updated entry back to disk
    int update_res = fat_dir_io_update_entry(fs, entry_dir_cluster, entry_offset, entry);
    if (update_res != FS_SUCCESS) {
        FAT_ERROR_LOG("Failed to update directory entry after truncation");
        return update_res;
    }

    FAT_DEBUG_LOG("File truncated successfully");
    return FS_SUCCESS;
}

vnode_t *fat_dir_operations_create_vnode(fat_fs_t *fs, const fat_dir_entry_t *entry,
                                        uint32_t entry_dir_cluster, uint32_t entry_offset,
                                        bool was_created, bool was_truncated)
{
    KERNEL_ASSERT(fs && entry, "Invalid parameters to create_vnode");

    // Allocate vnode and context
    vnode_t *vnode = kmalloc(sizeof(vnode_t));
    fat_file_context_t *file_ctx = kmalloc(sizeof(fat_file_context_t));
    if (!vnode || !file_ctx) {
        FAT_ERROR_LOG("kmalloc failed (vnode=%p, file_ctx=%p). Out of memory", vnode, file_ctx);
        if (vnode) kfree(vnode);
        if (file_ctx) kfree(file_ctx);
        return NULL;
    }
    
    memset(vnode, 0, sizeof(*vnode));
    memset(file_ctx, 0, sizeof(*file_ctx));
    FAT_DEBUG_LOG("Allocation successful: vnode=%p, file_ctx=%p", vnode, file_ctx);

    // Populate context
    FAT_DEBUG_LOG("Populating file context...");
    uint32_t first_cluster_final = fat_get_entry_cluster(entry);
    file_ctx->fs                  = fs;
    file_ctx->first_cluster       = first_cluster_final;
    file_ctx->file_size           = entry->file_size;
    file_ctx->dir_entry_cluster   = entry_dir_cluster;
    file_ctx->dir_entry_offset    = entry_offset;
    file_ctx->is_directory        = (entry->attr & FAT_ATTR_DIRECTORY);
    file_ctx->dirty               = (was_created || was_truncated);
    file_ctx->readdir_current_cluster = file_ctx->is_directory ? first_cluster_final : 0;
    
    if (fs->type != FAT_TYPE_FAT32 && file_ctx->is_directory && file_ctx->first_cluster == 0) {
        file_ctx->readdir_current_cluster = 0;
    }
    
    file_ctx->readdir_current_offset = 0;
    file_ctx->readdir_last_index = (size_t)-1;
    
    FAT_DEBUG_LOG("Context populated: first_cluster=%lu, size=%lu, is_dir=%d, dirty=%d",
                  (unsigned long)file_ctx->first_cluster, (unsigned long)file_ctx->file_size,
                  file_ctx->is_directory, file_ctx->dirty);

    // Link context to vnode
    FAT_DEBUG_LOG("Linking context %p to vnode %p...", file_ctx, vnode);
    vnode->data = file_ctx;
    vnode->fs_driver = &fat_vfs_driver;

    return vnode;
}