/**
 * @file fat_path_resolver.h
 * @brief FAT filesystem path resolution interface
 */

#ifndef KERNEL_FS_FAT_PATH_RESOLVER_H
#define KERNEL_FS_FAT_PATH_RESOLVER_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <kernel/fs/fat/fat_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lookup a path in the FAT filesystem
 * 
 * @param fs FAT filesystem context
 * @param path Path to lookup
 * @param entry_out Output directory entry
 * @param lfn_buffer_out Output buffer for long filename
 * @param entry_dir_cluster_out Output directory cluster containing the entry
 * @param entry_offset_in_dir_out Output offset within directory
 * @return 0 on success, negative error code on failure
 */
int fat_lookup_path(fat_fs_t *fs, const char *path,
                   fat_dir_entry_t *entry_out,
                   char *lfn_buffer_out,
                   uint32_t *entry_dir_cluster_out,
                   uint32_t *entry_offset_in_dir_out);

/**
 * @brief Find an entry in a directory
 * 
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster to search
 * @param name Name to find
 * @param entry_out Output directory entry
 * @param lfn_buffer_out Output buffer for long filename
 * @param entry_dir_cluster_out Output directory cluster
 * @param entry_offset_in_dir_out Output offset within directory
 * @return 0 on success, negative error code on failure
 */
int fat_find_in_dir(fat_fs_t *fs,
                   uint32_t dir_cluster,
                   const char *name,
                   fat_dir_entry_t *entry_out,
                   char *lfn_buffer_out,
                   uint32_t *entry_dir_cluster_out,
                   uint32_t *entry_offset_in_dir_out);

/**
 * @brief Validate a path for FAT filesystem
 * 
 * @param path Path to validate
 * @return 0 if valid, negative error code if invalid
 */
int fat_validate_path(const char *path);

/**
 * @brief Split a path into directory and filename components
 * 
 * @param path Full path to split
 * @param dir_out Output buffer for directory part
 * @param name_out Output buffer for filename part
 * @return 0 on success, negative error code on failure
 */
int fat_split_path(const char *path, char *dir_out, char *name_out);

/**
 * @brief Initialize path resolver
 * 
 * @return E_SUCCESS on success, error code on failure
 */
error_t fat_path_resolver_init(void);

/**
 * @brief Cleanup path resolver
 */
void fat_path_resolver_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_FS_FAT_PATH_RESOLVER_H