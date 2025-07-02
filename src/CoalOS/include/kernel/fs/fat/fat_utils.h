/**
 * @file fat_utils.h
 * @brief Utility functions for FAT filesystem driver (v1.1 - Corrected Signatures)
 *
 * Provides helper functions for cluster/LBA conversion, FAT table access,
 * filename formatting/comparison, timestamp retrieval, and short name generation.
 */

 #ifndef FAT_UTILS_H
 #define FAT_UTILS_H
 
 #include <kernel/core/types.h>      // Core types (uint*_t, size_t, bool, etc.)
 #include <kernel/fs/fat/fat_core.h>   // For fat_fs_t definition
 #include <kernel/fs/fat/fat_fs.h>     // For fat_dir_entry_t definition (needed by some helpers)
 #include <kernel/fs/vfs/fs_errno.h>   // For FS_ERR_* codes
 
 // --- Standard Libc Includes (Ensure paths are correct for your build) ---
 #include <libc/stdbool.h>
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 // --- FAT Geometry and FAT Table Access ---
 
 /**
  * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
  * @param fs Pointer to the FAT filesystem context.
  * @param cluster Cluster number (must be >= 2).
  * @return The starting LBA of the cluster, or 0 if the cluster number is invalid.
  */
 uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster);
 
 /**
  * @brief Retrieves the next cluster number in a file's allocation chain from the FAT table.
  * @param fs Pointer to the FAT filesystem context.
  * @param current_cluster The cluster number whose entry should be read.
  * @param next_cluster Output pointer to store the value read from the FAT entry.
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code on failure (e.g., IO error, invalid format).
  */
 int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster);
 
 /**
  * @brief Sets the value of a specific cluster entry in the in-memory FAT table.
  * Marks the FAT table as dirty.
  * @param fs Pointer to the FAT filesystem context.
  * @param cluster The cluster number whose entry should be set (must be >= 2).
  * @param value The new value to write (e.g., next cluster number, EOC marker, 0 for free).
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code on failure (e.g., invalid cluster, invalid format).
  */
 int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);
 
 /**
  * @brief Retrieves the raw value of a specific cluster entry from the in-memory FAT table.
  * @param fs Pointer to the FAT filesystem context.
  * @param cluster The cluster number whose entry should be read.
  * @param entry_value Output pointer to store the raw value read from the FAT entry (includes reserved bits for FAT32).
  * @return FS_SUCCESS on success, or a negative FS_ERR_* code on failure (e.g., invalid cluster, invalid format).
  */
 int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value);
 
 
 // --- Filename Formatting and Comparison ---
 
 /**
  * @brief Converts a standard filename string into the 11-byte FAT 8.3 format.
  * Handles padding, uppercase conversion, and illegal character replacement.
  * @param input The input filename string (long or short).
  * @param output_8_3 Buffer of exactly 11 bytes to store the formatted 8.3 name.
  */
 void format_filename(const char *input, char output_8_3[11]);
 
 /**
  * @brief Compares a filename component string against a reconstructed LFN string (case-insensitive).
  * @param component The filename component to compare.
  * @param reconstructed_lfn The LFN string reconstructed from directory entries.
  * @return 0 if strings match (case-insensitive), <0 if component < lfn, >0 if component > lfn.
  */
 int fat_compare_lfn(const char* component, const char* reconstructed_lfn);
 
 /**
  * @brief Compares a filename component string against a raw 11-byte 8.3 FAT filename (case-insensitive).
  * Formats the component string into 8.3 format first before comparing.
  * @param component The filename component to compare.
  * @param name_8_3 The raw 11-byte 8.3 name from a directory entry.
  * @return 0 if names match, non-zero otherwise (result of memcmp).
  */
 int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]);
 
 
 // --- Timestamp ---
 
 /**
  * @brief Gets the current time and date in FAT timestamp format.
  * @note The implementation currently uses a fixed date/time. Replace with RTC/PIT logic.
  * @param fat_time Output pointer for the 16-bit FAT time value.
  * @param fat_date Output pointer for the 16-bit FAT date value.
  */
 void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date);
 
 
 // --- Short Name Generation & Collision Check ---
 
 /**
  * @brief Checks if a directory entry with the exact raw 11-byte short name exists within a directory.
  * Scans the directory cluster chain.
  * @param fs Filesystem context.
  * @param dir_cluster Starting cluster of the directory to search (0 for FAT12/16 root).
  * @param short_name_raw The 11-byte raw 8.3 name (uppercase, space-padded) to check for.
  * @return true if the exact raw name exists, false otherwise or on I/O error.
  * @note Assumes caller holds fs->lock if concurrent modification is possible.
  */
 bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]);
 
 /**
  * @brief Generates a unique 8.3 short filename based on a long filename, checking for collisions.
  * Implements the standard "NAME~N.EXT" generation algorithm.
  * @param fs Filesystem context (used for uniqueness check via fat_raw_short_name_exists).
  * @param parent_dir_cluster The cluster number of the parent directory where the name will be created.
  * @param long_name The desired long filename.
  * @param short_name_out Buffer of 11 bytes to store the generated unique 8.3 name.
  * @return FS_SUCCESS (0) if a unique name was generated, or a negative FS_ERR_* code on failure (e.g., no unique name found).
  * @note Assumes caller holds fs->lock.
  */
 int fat_generate_short_name(fat_fs_t   *fs,
                             uint32_t    parent_dir_cluster, // <-- Added parameter
                             const char *long_name,
                             uint8_t     short_name_out[11]);
 
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // FAT_UTILS_H