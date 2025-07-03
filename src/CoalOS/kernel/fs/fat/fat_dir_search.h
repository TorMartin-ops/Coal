/**
 * @file fat_dir_search.h
 * @brief Directory Entry Search and LFN Matching for FAT Filesystem
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Handles searching for specific directory entries within directory
 * clusters, including Long File Name (LFN) sequence matching and reconstruction.
 * Focuses purely on search algorithms without path resolution.
 */

#ifndef FAT_DIR_SEARCH_H
#define FAT_DIR_SEARCH_H

//============================================================================
// Includes
//============================================================================
#include <kernel/fs/fat/fat_core.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Directory Search Functions
//============================================================================

/**
 * @brief Find a specific directory entry within a directory cluster
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
int fat_dir_search_find_in_dir(fat_fs_t *fs,
                               uint32_t dir_cluster,
                               const char *component,
                               fat_dir_entry_t *entry_out,
                               char *lfn_out, size_t lfn_max_len,
                               uint32_t *entry_offset_in_dir_out,
                               uint32_t *first_lfn_offset_out);

/**
 * @brief Find free directory slots for new entries
 * @param fs FAT filesystem context
 * @param parent_dir_cluster Parent directory cluster
 * @param needed_slots Number of consecutive slots needed
 * @param out_slot_cluster Output: cluster containing free slots
 * @param out_slot_offset Output: offset of first free slot
 * @return FS_SUCCESS if found, FS_ERR_NO_SPACE if not enough space, or error
 */
int fat_dir_search_find_free_slots(fat_fs_t *fs,
                                   uint32_t parent_dir_cluster,
                                   size_t needed_slots,
                                   uint32_t *out_slot_cluster,
                                   uint32_t *out_slot_offset);

//============================================================================
// Internal Helper Functions
//============================================================================

/**
 * @brief Read a single directory sector with proper cluster traversal
 * @param fs FAT filesystem context
 * @param cluster Starting cluster (0 for FAT12/16 root)
 * @param sector_offset_in_chain Sector offset within directory chain
 * @param buffer Output buffer for sector data
 * @return FS_SUCCESS on success, error code on failure
 */
int fat_dir_search_read_sector(fat_fs_t *fs, uint32_t cluster,
                              uint32_t sector_offset_in_chain,
                              uint8_t* buffer);

#endif // FAT_DIR_SEARCH_H