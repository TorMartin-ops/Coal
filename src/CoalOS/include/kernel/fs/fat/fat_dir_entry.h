/**
 * @file fat_dir_entry.h
 * @brief FAT Directory Entry Manipulation Interface
 * 
 * Provides functions for low-level directory entry operations including
 * marking entries as deleted, writing new entries, and finding free slots.
 */

#ifndef KERNEL_FS_FAT_DIR_ENTRY_H
#define KERNEL_FS_FAT_DIR_ENTRY_H

#include <kernel/core/types.h>
#include <kernel/fs/fat/fat_core.h>

/**
 * @brief Marks directory entries as deleted (sets first byte to 0xE5)
 * @param fs FAT filesystem structure
 * @param dir_cluster Starting cluster of the directory
 * @param start_entry_index Index of first entry to mark as deleted
 * @param num_entries Number of consecutive entries to mark as deleted
 * @return 0 on success, negative error code on failure
 */
int mark_directory_entries_deleted(fat_fs_t *fs,
                                  uint32_t dir_cluster,
                                  uint32_t start_entry_index,
                                  uint32_t num_entries);

/**
 * @brief Writes directory entries to a directory cluster
 * @param fs FAT filesystem structure
 * @param dir_cluster Starting cluster of the directory
 * @param start_entry_index Index where to start writing entries
 * @param entries Array of directory entries to write
 * @param num_entries Number of entries to write
 * @return 0 on success, negative error code on failure
 */
int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                           uint32_t start_entry_index,
                           const fat_dir_entry_t *entries,
                           uint32_t num_entries);

/**
 * @brief Finds a free slot in a directory for new entries
 * @param fs FAT filesystem structure
 * @param dir_cluster Starting cluster of the directory
 * @param num_entries_needed Number of consecutive free entries needed
 * @param out_entry_index Output parameter for the index of the first free entry
 * @return 0 on success, negative error code on failure
 */
int find_free_directory_slot(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t num_entries_needed,
                            uint32_t *out_entry_index);

#endif // KERNEL_FS_FAT_DIR_ENTRY_H