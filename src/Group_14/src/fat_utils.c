#include "fat_utils.h"
#include "terminal.h" // For debugging output if needed
#include <string.h>  // May need basic string functions if not provided elsewhere
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
        // Cannot return error message here easily without terminal access being standard
        // Consider returning an invalid LBA like 0 or -1 (if return type changed)
        return 0;
    }
    // Cluster 2 starts at the first data sector.
    return fs->first_data_sector + ((cluster - 2) * fs->boot_sector.sectors_per_cluster);
}

int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster) {
    if (!fs || !fs->fat_table || !next_cluster || current_cluster >= (fs->fat_size * fs->boot_sector.bytes_per_sector / (fs->type == FAT_TYPE_FAT16 ? 2 : 4)) ) {
         // Added bounds check for current_cluster
        return -1; // Invalid parameters or cluster out of bounds
    }
    if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *fat = (uint16_t *)fs->fat_table;
        *next_cluster = fat[current_cluster];
        // Check for FAT16 EOC markers (0xFFF8 - 0xFFFF)
        if (*next_cluster >= 0xFFF8) *next_cluster = FAT32_EOC; // Treat as EOC for consistency if needed
    } else if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *fat = (uint32_t *)fs->fat_table;
        *next_cluster = fat[current_cluster] & 0x0FFFFFFF;  // lower 28 bits
        // Check for FAT32 EOC markers (>= 0x0FFFFFF8)
         if (*next_cluster >= 0x0FFFFFF8) *next_cluster = FAT32_EOC;
    } else if (fs->type == FAT_TYPE_FAT12) {
        // FAT12 reading is complex due to 12-bit entries spanning byte boundaries
        return -2; // Not implemented
    } else {
        return -3; // Unknown FAT type
    }
    return 0;
}

int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->fat_table || cluster >= (fs->fat_size * fs->boot_sector.bytes_per_sector / (fs->type == FAT_TYPE_FAT16 ? 2 : 4))) {
        // Added bounds check
        return -1; // Invalid parameters or cluster out of bounds
    }
    if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *fat = (uint16_t *)fs->fat_table;
        fat[cluster] = (uint16_t)value;
    } else if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *fat = (uint32_t *)fs->fat_table;
        // Preserve upper 4 bits, update lower 28
        fat[cluster] = (fat[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } else if (fs->type == FAT_TYPE_FAT12) {
        // FAT12 writing is complex
        return -2; // Not implemented
    } else {
        return -3; // Unknown FAT type
    }
    // TODO: Consider marking the FAT buffer dirty for later flushing
    return 0;
}


/**
 * @brief Converts a standard filename to FAT 8.3 format.
 *
 * Converts the filename part (before '.') to uppercase, pads with spaces to 8 chars.
 * Converts the extension part (after '.') to uppercase, pads with spaces to 3 chars.
 * Handles cases with no extension, or names/extensions shorter/longer than limits.
 * Does not handle invalid characters or LFNs.
 *
 * @param input The null-terminated input filename (e.g., "file.txt").
 * @param output_8_3 A character array of exactly 11 bytes to store the result
 * (e.g., "FILE    TXT"). The output is NOT null-terminated by this function.
 */
void format_filename(const char *input, char output_8_3[11]) {
    // Initialize output with spaces
    for (int i = 0; i < 11; i++) {
        output_8_3[i] = ' ';
    }

    const char *dot = strrchr(input, '.'); // Find last dot
    size_t name_len;
    const char *ext_start = NULL;

    if (dot == NULL) {
        // No dot found, entire string is the name
        name_len = strlen(input);
    } else {
        // Dot found, calculate name length
        name_len = dot - input;
        ext_start = dot + 1; // Point to character after dot
    }

    // Copy and uppercase the name part (max 8 chars)
    for (size_t i = 0; i < 8 && i < name_len; i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') {
            output_8_3[i] = c - 'a' + 'A'; // Convert to uppercase
        } else {
            // Copy other characters as-is (basic implementation)
            // TODO: Handle potentially invalid FAT characters
            output_8_3[i] = c;
        }
    }

    // Copy and uppercase the extension part (max 3 chars)
    if (ext_start != NULL) {
        size_t ext_len = strlen(ext_start);
        for (size_t i = 0; i < 3 && i < ext_len; i++) {
            char c = ext_start[i];
            if (c >= 'a' && c <= 'z') {
                output_8_3[8 + i] = c - 'a' + 'A'; // Convert to uppercase
            } else {
                // Copy other characters as-is
                // TODO: Handle potentially invalid FAT characters
                output_8_3[8 + i] = c;
            }
        }
    }
    // The rest of output_8_3 remains padded with spaces from initialization.
}