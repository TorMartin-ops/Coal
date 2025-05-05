#ifndef FAT_UTILS_H
#define FAT_UTILS_H

#include "types.h"
#include "fat_core.h"   // For fat_fs_t
#include "fat_fs.h"     // For fat_dir_entry_t (Needed for read_directory_sector decl)
#include <libc/stdbool.h> // For bool (assuming path)
#include <libc/stdint.h>  // For uint*_t (assuming path)
#include <libc/stddef.h>  // For size_t (assuming path)

#ifdef __cplusplus
extern "C" {
#endif

// --- FAT Geometry and FAT Table Access ---
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster);
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster);
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);
int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value);

// --- Filename Formatting and Comparison ---
void format_filename(const char *input, char output_8_3[11]); // Standard 8.3 conversion
int fat_compare_lfn(const char* component, const char* reconstructed_lfn); // Case-insensitive LFN compare
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]); // 8.3 raw name comparison

// --- Timestamp ---
void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date);

// --- Short Name Generation & Collision Check ---

/**
 * @brief Checks if a directory entry with the exact raw 11-byte short name exists.
 * @note Implementation is expected in fat_utils.c (or fat_dir.c). Assumes caller holds fs lock if needed.
 *
 * @param fs Filesystem context.
 * @param dir_cluster Cluster of the directory to search (0 for FAT12/16 root).
 * @param short_name_raw The 11-byte raw 8.3 name to check for.
 * @return true if the exact raw name exists, false otherwise or on error.
 */
bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]);

/**
 * @brief Generates a unique 8.3 short filename based on a long filename.
 * Tries to create a base name and adds numeric tails (~1, ~2...) if necessary.
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param parent_dir_cluster The cluster number of the parent directory (0 for FAT12/16 root).
 * @param long_name The desired long filename.
 * @param short_name_out Buffer of 11 bytes to store the generated unique 8.3 name.
 * @return FS_SUCCESS (0) if a unique name was generated, or a negative FS_ERR_* code on failure.
 */
int fat_generate_short_name(fat_fs_t *fs,
                             uint32_t parent_dir_cluster,
                             const char* long_name,
                             uint8_t short_name_out[11]);


// --- Directory Sector Reading ---
// NOTE: Implemented in fat_dir.c, declared here as requested.
// Consider moving declaration to fat_dir.h for better organization.
/**
 * @brief Reads a specific sector from a directory structure (root or sub-directory).
 * @param fs Filesystem context.
 * @param cluster Starting cluster of the directory (0 for FAT12/16 root).
 * @param sector_offset_in_chain Sector index relative to the start of the directory.
 * @param buffer Output buffer (must be at least fs->bytes_per_sector large).
 * @return FS_SUCCESS on success, negative FS_ERR_* code on failure.
 */


#ifdef __cplusplus
}
#endif

#endif // FAT_UTILS_H