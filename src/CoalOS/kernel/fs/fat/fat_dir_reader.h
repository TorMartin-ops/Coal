/**
 * @file fat_dir_reader.h
 * @brief Directory Reading and Iteration for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles directory reading operations including readdir functionality,
 * LFN reconstruction, and directory entry iteration. Manages directory traversal
 * state and provides dirent structures to VFS layer.
 */

#ifndef FAT_DIR_READER_H
#define FAT_DIR_READER_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/core/types.h>
#include <libc/stdint.h>

//============================================================================
// Directory Reading Functions
//============================================================================

/**
 * @brief Read directory entries and populate dirent structure
 * @param dir_file File handle for directory being read
 * @param d_entry_out Output buffer for directory entry information
 * @param entry_index Logical index of entry to read (0-based)
 * @return FS_SUCCESS if entry found, FS_ERR_NOT_FOUND if no more entries, or error code
 */
int fat_dir_reader_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, 
                                    size_t entry_index);

//============================================================================
// Internal Helper Functions
//============================================================================

/**
 * @brief Reset directory reading state to beginning
 * @param file_ctx FAT file context for directory
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_reader_reset_state(fat_file_context_t *file_ctx);

/**
 * @brief Format 8.3 name into readable string
 * @param name_8_3 Raw 8.3 name from directory entry
 * @param out_name Output buffer for formatted name
 */
void fat_dir_reader_format_short_name(const uint8_t name_8_3[11], char *out_name);

/**
 * @brief Advance directory reading position to next cluster
 * @param fs FAT filesystem context
 * @param file_ctx Directory file context
 * @return FS_SUCCESS on success, FS_ERR_NOT_FOUND at end, or error code
 */
int fat_dir_reader_advance_cluster(fat_fs_t *fs, fat_file_context_t *file_ctx);

#endif // FAT_DIR_READER_H