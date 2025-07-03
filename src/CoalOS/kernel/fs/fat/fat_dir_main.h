/**
 * @file fat_dir_main.h
 * @brief Main Directory Operations Coordination for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Serves as the main coordination point for all directory-related
 * operations in the FAT filesystem. Provides the public API and delegates
 * to specialized modules following the Facade pattern.
 */

#ifndef FAT_DIR_MAIN_H
#define FAT_DIR_MAIN_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/core/types.h>
#include <libc/stdint.h>

//============================================================================
// Public API - Facade Pattern
//============================================================================

/**
 * @brief Open or create a file/directory within the FAT filesystem
 * @param fs_context FAT filesystem context
 * @param path Path to file/directory to open
 * @param flags Open flags (O_CREAT, O_TRUNC, etc.)
 * @return Pointer to allocated vnode on success, NULL on failure
 */
vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);

/**
 * @brief Read directory entries and populate dirent structure
 * @param dir_file File handle for directory being read
 * @param d_entry_out Output buffer for directory entry information
 * @param entry_index Logical index of entry to read (0-based)
 * @return FS_SUCCESS if entry found, FS_ERR_NOT_FOUND if no more entries, or error code
 */
int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index);

/**
 * @brief Unlink (delete) a file from the FAT filesystem
 * @param fs_context FAT filesystem context
 * @param path Path to file to delete
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_unlink_internal(void *fs_context, const char *path);

/**
 * @brief Find a directory entry by name within a directory
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster to search (0 for FAT12/16 root)
 * @param component Name component to search for
 * @param entry_out Output buffer for found directory entry
 * @param lfn_out Output buffer for reconstructed long filename (optional)
 * @param lfn_max_len Maximum length of LFN output buffer
 * @param entry_offset_in_dir_out Output: absolute offset of found entry
 * @param first_lfn_offset_out Output: offset of first LFN entry (optional)
 * @return FS_SUCCESS if found, FS_ERR_NOT_FOUND if not found, or error code
 */
int fat_find_in_dir(fat_fs_t *fs,
                   uint32_t dir_cluster,
                   const char *component,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_offset_in_dir_out,
                   uint32_t *first_lfn_offset_out);

/**
 * @brief Resolve a full path to a directory entry
 * @param fs FAT filesystem context
 * @param path Full path to resolve
 * @param entry_out Output buffer for found directory entry
 * @param lfn_out Output buffer for long filename (optional)
 * @param lfn_max_len Maximum length of LFN output buffer
 * @param entry_dir_cluster_out Output: cluster containing the entry
 * @param entry_offset_in_dir_out Output: offset of entry within directory
 * @return FS_SUCCESS if found, FS_ERR_NOT_FOUND if not found, or error code
 */
int fat_lookup_path(fat_fs_t *fs, const char *path,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_dir_cluster_out,
                   uint32_t *entry_offset_in_dir_out);

//============================================================================
// Legacy Support Functions (for compatibility)
//============================================================================

/**
 * @brief Read a directory sector with proper cluster traversal
 * @param fs FAT filesystem context
 * @param cluster Starting cluster (0 for FAT12/16 root)
 * @param sector_offset_in_chain Sector offset within directory chain
 * @param buffer Output buffer for sector data
 * @return FS_SUCCESS on success, error code on failure
 */
int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                          uint32_t sector_offset_in_chain,
                          uint8_t* buffer);

/**
 * @brief Update a directory entry on disk
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster containing the entry
 * @param dir_offset Byte offset of entry within directory
 * @param new_entry Updated directory entry data
 * @return FS_SUCCESS on success, error code on failure
 */
int update_directory_entry(fat_fs_t *fs,
                           uint32_t dir_cluster,
                           uint32_t dir_offset,
                           const fat_dir_entry_t *new_entry);

/**
 * @brief Mark directory entries as deleted
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster containing entries
 * @param first_entry_offset Offset of first entry to mark
 * @param num_entries Number of consecutive entries to mark
 * @param marker Deletion marker (usually FAT_DIR_ENTRY_DELETED)
 * @return FS_SUCCESS on success, error code on failure
 */
int mark_directory_entries_deleted(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t first_entry_offset,
                                   size_t num_entries,
                                   uint8_t marker);

/**
 * @brief Write multiple directory entries to disk
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster to write to
 * @param dir_offset Starting byte offset within directory
 * @param entries_buf Buffer containing directory entries
 * @param num_entries Number of entries to write
 * @return FS_SUCCESS on success, error code on failure
 */
int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const void *entries_buf,
                            size_t num_entries);

/**
 * @brief Find free directory slots for new entries
 * @param fs FAT filesystem context
 * @param parent_dir_cluster Parent directory cluster
 * @param needed_slots Number of consecutive slots needed
 * @param out_slot_cluster Output: cluster containing free slots
 * @param out_slot_offset Output: offset of first free slot
 * @return FS_SUCCESS if found, FS_ERR_NO_SPACE if not enough space, or error
 */
int find_free_directory_slot(fat_fs_t *fs,
                             uint32_t parent_dir_cluster,
                             size_t needed_slots,
                             uint32_t *out_slot_cluster,
                             uint32_t *out_slot_offset);

#endif // FAT_DIR_MAIN_H