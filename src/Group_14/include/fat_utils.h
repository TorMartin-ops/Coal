#pragma once
#ifndef FAT_UTILS_H
#define FAT_UTILS_H

#include "types.h"
#include "fat_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
 *
 * In FAT filesystems, the first data cluster (cluster 2) starts at fs->first_data_sector.
 * This function calculates the starting LBA for a given cluster.
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param cluster Cluster number (must be >= 2).
 * @return The LBA address for the start of the cluster, or 0 if the cluster number is invalid.
 */
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster);

/**
 * @brief Retrieves the next cluster in the chain from the FAT table.
 *
 * For FAT16, each FAT entry is 16 bits; for FAT32, each entry is 32 bits with the upper 4 bits reserved.
 * FAT12 is not fully supported in this demo.
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param current_cluster The current cluster number.
 * @param next_cluster Output pointer where the next cluster number will be stored.
 * @return 0 on success, or a negative error code on failure.
 */
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster);

/**
 * @brief Updates the FAT entry for a given cluster with a new value.
 *
 * For FAT16 and FAT32. For FAT32, only the lower 28 bits are modified.
 * FAT12 is not supported in this demo.
 *
 * @param fs Pointer to the FAT filesystem context.
 * @param cluster The cluster number to update.
 * @param value The new value to write into the FAT entry.
 * @return 0 on success, or a negative error code on failure.
 */
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);

/**
 * @brief Converts a standard filename to FAT 8.3 format.
 *
 * Converts the filename part (before '.') to uppercase, pads with spaces to 8 chars.
 * Converts the extension part (after '.') to uppercase, pads with spaces to 3 chars.
 * Handles cases with no extension, or names/extensions shorter/longer than limits.
 *
 * @param input The null-terminated input filename (e.g., "file.txt").
 * @param output_8_3 A character array of exactly 11 bytes to store the result
 * (e.g., "FILE    TXT"). The output is NOT null-terminated by this function.
 */
void format_filename(const char *input, char output_8_3[11]);

int fat_compare_lfn(const char* component, const char* reconstructed_lfn);
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]);
int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value);

#ifdef __cplusplus
}
#endif

#endif // FAT_UTILS_H