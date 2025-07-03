/**
 * @file fat_path_resolver.h
 * @brief Path Resolution and Component Navigation for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles path parsing, component traversal, and directory navigation.
 * Focuses purely on resolving filesystem paths to directory entries without
 * performing file operations.
 */

#ifndef FAT_PATH_RESOLVER_H
#define FAT_PATH_RESOLVER_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Path Resolution Functions
//============================================================================

/**
 * @brief Resolve a full path to a directory entry
 * @param fs FAT filesystem context
 * @param path Full path to resolve (e.g., "/dir/file.txt")
 * @param entry_out Output buffer for found directory entry
 * @param lfn_out Output buffer for long filename (optional)
 * @param lfn_max_len Maximum length of LFN output buffer
 * @param entry_dir_cluster_out Output: cluster containing the entry
 * @param entry_offset_in_dir_out Output: offset of entry within directory
 * @return FS_SUCCESS if found, FS_ERR_NOT_FOUND if not found, or error code
 */
int fat_path_resolver_lookup(fat_fs_t *fs, const char *path,
                            fat_dir_entry_t *entry_out,
                            char *lfn_out, size_t lfn_max_len,
                            uint32_t *entry_dir_cluster_out,
                            uint32_t *entry_offset_in_dir_out);

/**
 * @brief Get directory entry information for root directory
 * @param fs FAT filesystem context
 * @param entry_out Output buffer for root directory entry
 * @param lfn_out Output buffer for long filename (optional)
 * @param lfn_max_len Maximum length of LFN output buffer
 * @param entry_dir_cluster_out Output: cluster containing root entry (always 0)
 * @param entry_offset_in_dir_out Output: offset of root entry (always 0)
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_path_resolver_get_root_entry(fat_fs_t *fs, fat_dir_entry_t *entry_out,
                                    char *lfn_out, size_t lfn_max_len,
                                    uint32_t *entry_dir_cluster_out,
                                    uint32_t *entry_offset_in_dir_out);

/**
 * @brief Split a full path into parent directory and final component
 * @param full_path Full path to split (e.g., "/dir/file.txt")
 * @param parent_path Output buffer for parent path (e.g., "/dir")
 * @param parent_max Maximum length of parent path buffer
 * @param component_name Output buffer for final component (e.g., "file.txt")
 * @param component_max Maximum length of component name buffer
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_path_resolver_split_path(const char *full_path, char *parent_path, size_t parent_max,
                                char *component_name, size_t component_max);

/**
 * @brief Check if a path represents the root directory
 * @param path Path to check
 * @return true if path is root ("/", "", etc.), false otherwise
 */
bool fat_path_resolver_is_root_path(const char *path);

/**
 * @brief Validate a path component name
 * @param component Component name to validate
 * @return FS_SUCCESS if valid, FS_ERR_INVALID_PARAM if invalid
 */
int fat_path_resolver_validate_component(const char *component);

#endif // FAT_PATH_RESOLVER_H