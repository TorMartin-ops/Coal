#pragma once
#ifndef FAT_UTILS_H
#define FAT_UTILS_H

#include "types.h" 
#include "fat.h"

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
 * FAT12 is not supported in this demo.
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

#endif // FAT_UTILS_H
