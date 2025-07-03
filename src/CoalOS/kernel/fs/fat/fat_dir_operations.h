/**
 * @file fat_dir_operations.h
 * @brief File and Directory Operations for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles high-level file operations including opening with O_CREAT/O_TRUNC,
 * unlinking files, and directory management. Coordinates between path resolution,
 * search, and I/O operations.
 */

#ifndef FAT_DIR_OPERATIONS_H
#define FAT_DIR_OPERATIONS_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/vfs/vfs.h>
#include <libc/stdint.h>

//============================================================================
// File Operations Functions
//============================================================================

/**
 * @brief Open or create a file/directory within the FAT filesystem
 * @param fs_context FAT filesystem context
 * @param path Path to file/directory to open
 * @param flags Open flags (O_CREAT, O_TRUNC, etc.)
 * @return Pointer to allocated vnode on success, NULL on failure
 */
vnode_t *fat_dir_operations_open_internal(void *fs_context, const char *path, int flags);

/**
 * @brief Unlink (delete) a file from the FAT filesystem
 * @param fs_context FAT filesystem context
 * @param path Path to file to delete
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_operations_unlink_internal(void *fs_context, const char *path);

/**
 * @brief Truncate an existing file to zero size
 * @param fs FAT filesystem context
 * @param entry Directory entry for the file
 * @param entry_dir_cluster Cluster containing the directory entry
 * @param entry_offset Offset of the entry within the directory
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_operations_truncate_file(fat_fs_t *fs, fat_dir_entry_t *entry,
                                     uint32_t entry_dir_cluster, uint32_t entry_offset);

//============================================================================
// Internal Helper Functions
//============================================================================

/**
 * @brief Create a new vnode and file context for a FAT entry
 * @param fs FAT filesystem context
 * @param entry Directory entry information
 * @param entry_dir_cluster Cluster containing the directory entry
 * @param entry_offset Offset of the entry within the directory
 * @param was_created Whether the file was just created
 * @param was_truncated Whether the file was truncated
 * @return Pointer to new vnode on success, NULL on failure
 */
vnode_t *fat_dir_operations_create_vnode(fat_fs_t *fs, const fat_dir_entry_t *entry,
                                         uint32_t entry_dir_cluster, uint32_t entry_offset,
                                         bool was_created, bool was_truncated);

#endif // FAT_DIR_OPERATIONS_H