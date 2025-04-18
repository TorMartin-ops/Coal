#pragma once
#ifndef FAT_UTILS_H
#define FAT_UTILS_H

#include "types.h"
#include "fat_core.h"   // For fat_fs_t
#include "fat_fs.h"     // For fat_dir_entry_t (Needed if fat_raw_short_name_exists declared here)
#include "libc/stdbool.h" // For bool

#ifdef __cplusplus
extern "C" {
#endif

// --- Existing Declarations ---
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster);
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster);
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);
void format_filename(const char *input, char output_8_3[11]); // Note: name changed in .c, keep consistent
int fat_compare_lfn(const char* component, const char* reconstructed_lfn);
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]);
int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value);

// --- NEW Declarations ---

/**
 * @brief Generates a unique FAT 8.3 short filename from a long filename.
 * Tries to create a base name and adds a numeric tail (~1) if necessary.
 * NOTE: This is a simplified version and may fail if ~1 also exists.
 *
 * @param fs Filesystem context.
 * @param parent_dir_cluster Cluster number of the directory where the name will reside.
 * @param long_name The long filename input.
 * @param short_name_raw Output buffer (11 bytes) for the generated raw 8.3 name.
 * @return FS_SUCCESS on success, negative error code on failure (e.g., FS_ERR_EXISTS if unique name cannot be generated).
 */
int fat_generate_short_name(fat_fs_t *fs, uint32_t parent_dir_cluster, const char *long_name, uint8_t short_name_raw[11]);

/**
 * @brief Gets the current time and date in FAT timestamp format.
 * NOTE: This implementation returns a FIXED time. Replace with actual time source access.
 *
 * @param fat_time Output pointer for the 16-bit FAT time.
 * @param fat_date Output pointer for the 16-bit FAT date.
 */
void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date);

/**
 * @brief Checks if a directory entry with the exact raw 11-byte short name exists.
 * @note Implementation is expected in fat_dir.c. Assumes caller holds fs lock.
 *
 * @param fs Filesystem context.
 * @param dir_cluster Cluster of the directory to search.
 * @param short_name_raw The 11-byte raw 8.3 name to check for.
 * @return true if the exact raw name exists, false otherwise or on error.
 */
bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]);


#ifdef __cplusplus
}
#endif

#endif // FAT_UTILS_H