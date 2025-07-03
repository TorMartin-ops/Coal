/**
 * @file fat_dir_io.h
 * @brief Low-Level Directory I/O Operations for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles low-level directory sector I/O operations including reading,
 * writing, and updating directory entries. Manages buffer cache interactions
 * and provides sector-level directory access primitives.
 */

#ifndef FAT_DIR_IO_H
#define FAT_DIR_IO_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <kernel/drivers/storage/buffer_cache.h>
#include <libc/stdint.h>

//============================================================================
// Directory I/O Functions
//============================================================================

/**
 * @brief Update a directory entry on disk
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster containing the entry
 * @param dir_offset Byte offset of entry within directory
 * @param new_entry Updated directory entry data
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_io_update_entry(fat_fs_t *fs,
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
int fat_dir_io_mark_entries_deleted(fat_fs_t *fs,
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
int fat_dir_io_write_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries);

//============================================================================
// Internal Helper Functions
//============================================================================

/**
 * @brief Convert directory cluster and offset to LBA
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster (0 for FAT12/16 root)
 * @param sector_offset_in_chain Sector offset within directory chain
 * @param lba_out Output: calculated LBA
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_io_calculate_lba(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t sector_offset_in_chain,
                             uint32_t *lba_out);

/**
 * @brief Get buffer for directory sector with proper cluster traversal
 * @param fs FAT filesystem context
 * @param dir_cluster Directory cluster
 * @param sector_offset_in_chain Sector offset within directory
 * @param lba_out Output: calculated LBA for the sector
 * @return Buffer pointer on success, NULL on failure
 */
buffer_t* fat_dir_io_get_sector_buffer(fat_fs_t *fs, uint32_t dir_cluster,
                                       uint32_t sector_offset_in_chain,
                                       uint32_t *lba_out);

#endif // FAT_DIR_IO_H