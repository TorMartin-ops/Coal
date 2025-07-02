/**
 * @file fat_file_ops.h
 * @brief FAT filesystem file operations interface
 */

#ifndef KERNEL_FS_FAT_FILE_OPS_H
#define KERNEL_FS_FAT_FILE_OPS_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/vfs/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a file or directory in the FAT filesystem
 * 
 * @param fs_context FAT filesystem context
 * @param path Path to open
 * @param flags Open flags (O_CREAT, O_TRUNC, etc.)
 * @return Pointer to vnode on success, NULL on failure
 */
vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);

/**
 * @brief Close a file or directory
 * 
 * @param vnode VFS node to close
 * @return 0 on success, negative error code on failure
 */
int fat_close_internal(vnode_t *vnode);

/**
 * @brief Read from a file
 * 
 * @param vnode VFS node to read from
 * @param buffer Buffer to read into
 * @param size Number of bytes to read
 * @param offset Offset to read from
 * @return Number of bytes read on success, negative error code on failure
 */
ssize_t fat_read_internal(vnode_t *vnode, void *buffer, size_t size, off_t offset);

/**
 * @brief Write to a file
 * 
 * @param vnode VFS node to write to
 * @param buffer Buffer to write from
 * @param size Number of bytes to write
 * @param offset Offset to write to
 * @return Number of bytes written on success, negative error code on failure
 */
ssize_t fat_write_internal(vnode_t *vnode, const void *buffer, size_t size, off_t offset);

/**
 * @brief Create a new file
 * 
 * @param fs FAT filesystem context
 * @param path Path of file to create
 * @param attr File attributes
 * @param entry_out Output directory entry
 * @param dir_cluster_out Output directory cluster
 * @param dir_offset_out Output directory offset
 * @return 0 on success, negative error code on failure
 */
int fat_create_file(fat_fs_t *fs, const char *path, uint8_t attr,
                   fat_dir_entry_t *entry_out,
                   uint32_t *dir_cluster_out,
                   uint32_t *dir_offset_out);

/**
 * @brief Truncate a file to zero size
 * 
 * @param fs FAT filesystem context
 * @param entry Directory entry of file
 * @param dir_cluster Directory cluster containing entry
 * @param dir_offset Offset of entry in directory
 * @return 0 on success, negative error code on failure
 */
int fat_truncate_file(fat_fs_t *fs, fat_dir_entry_t *entry,
                     uint32_t dir_cluster, uint32_t dir_offset);

/**
 * @brief Delete a file
 * 
 * @param fs_context FAT filesystem context
 * @param path Path of file to delete
 * @return 0 on success, negative error code on failure
 */
int fat_unlink_internal(void *fs_context, const char *path);

/**
 * @brief Initialize FAT file operations
 * 
 * @return E_SUCCESS on success, error code on failure
 */
error_t fat_file_ops_init(void);

/**
 * @brief Cleanup FAT file operations
 */
void fat_file_ops_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_FS_FAT_FILE_OPS_H