#include "fat_utils.h"
#include "terminal.h"
#include <string.h>
#include "types.h"

/*
 * Note:
 * - It is assumed that fs->fat_table has already been loaded into memory (via fat_mount or similar).
 * - For FAT16, each FAT entry occupies 2 bytes.
 * - For FAT32, each FAT entry occupies 4 bytes, but only the lower 28 bits are used.
 * - FAT12 is not fully implemented in this demo.
 */

uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster) {
    if (!fs || cluster < 2) {
        terminal_write("[FAT Utils] Invalid parameters for fat_cluster_to_lba.\n");
        return 0;
    }
    // Cluster 2 starts at the first data sector.
    return fs->first_data_sector + ((cluster - 2) * fs->boot_sector.sectors_per_cluster);
}

int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster) {
    if (!fs || !fs->fat_table || !next_cluster) {
        terminal_write("[FAT Utils] fat_get_next_cluster: Invalid parameters.\n");
        return -1;
    }
    if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *fat = (uint16_t *)fs->fat_table;
        *next_cluster = fat[current_cluster];
    } else if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *fat = (uint32_t *)fs->fat_table;
        *next_cluster = fat[current_cluster] & 0x0FFFFFFF;  // lower 28 bits
    } else if (fs->type == FAT_TYPE_FAT12) {
        terminal_write("[FAT Utils] FAT12 support is not implemented.\n");
        return -2;
    } else {
        terminal_write("[FAT Utils] Unknown FAT type.\n");
        return -3;
    }
    return 0;
}

int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->fat_table) {
        terminal_write("[FAT Utils] fat_set_cluster_entry: Invalid parameters.\n");
        return -1;
    }
    if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *fat = (uint16_t *)fs->fat_table;
        fat[cluster] = (uint16_t)value;
    } else if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *fat = (uint32_t *)fs->fat_table;
        fat[cluster] = (fat[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } else if (fs->type == FAT_TYPE_FAT12) {
        terminal_write("[FAT Utils] FAT12 support is not implemented.\n");
        return -2;
    } else {
        terminal_write("[FAT Utils] Unknown FAT type.\n");
        return -3;
    }
    return 0;
}
