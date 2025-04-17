#include "fat_core.h"
#include "fat_utils.h"
#include <string.h>
#include "libc/ctype.h"

/**
 * Convert a FAT cluster number to LBA (Logical Block Address)
 * for disk I/O operations.
 */
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster)
{
    if (!fs || cluster < 2) return 0; // Invalid cluster (FAT clusters start at 2)
    
    // Calculate the LBA from the first data sector and the cluster number
    // Note: Clusters are 0-based for LBA calculation after subtracting the reserved 2 entries
    return fs->first_data_sector + ((cluster - 2) * fs->sectors_per_cluster);
}

/**
 * Get the next cluster in a chain from the FAT table.
 */
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster)
{
    if (!fs || !fs->fat_table || !next_cluster || 
        current_cluster >= (fs->fat_size * fs->bytes_per_sector / (fs->type == FAT_TYPE_FAT16 ? 2 : 4))) {
        return -1; // Invalid parameters
    }

    // Access the appropriate FAT table based on filesystem type
    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        *next_cluster = FAT32[current_cluster] & 0x0FFFFFFF; // Mask reserved bits
    } 
    else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        *next_cluster = FAT16[current_cluster];
    }
    else {
        // FAT12 not fully implemented in this example
        return -1;
    }
    
    return 0; // Success
}

/**
 * Set/Update a cluster entry in the FAT table.
 */
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value)
{
    if (!fs || !fs->fat_table || 
        cluster >= (fs->fat_size * fs->bytes_per_sector / (fs->type == FAT_TYPE_FAT16 ? 2 : 4))) {
        return -1; // Invalid parameters
    }

    // Update the appropriate FAT table based on filesystem type
    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        // Preserve the top 4 bits (reserved)
        FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } 
    else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        FAT16[cluster] = (uint16_t)value;
    }
    else {
        // FAT12 not fully implemented in this example
        return -1;
    }
    
    return 0; // Success
}

/**
 * Format/normalize a filename to FAT 8.3 format.
 */
void format_filename(const char *input, char *output)
{
    size_t i, j;
    
    // Initialize output with spaces (padding for 8.3 format)
    memset(output, ' ', 11);
    
    // Handle empty input
    if (!input || input[0] == '\0') {
        memcpy(output, "NO_NAME    ", 11);
        return;
    }
    
    // Skip leading dots and spaces
    while (*input && (*input == '.' || *input == ' ')) {
        input++;
    }
    
    // Handle empty input after skipping
    if (!*input) {
        memcpy(output, "NO_NAME    ", 11);
        return;
    }

    // Process the filename part (up to 8 characters before the extension)
    for (i = 0, j = 0; input[i] && input[i] != '.' && j < 8; i++) {
        char c = input[i];
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || 
            c == '"' || c == '<' || c == '>' || c == '|' || c < ' ') {
            continue; // Skip invalid characters
        }
        output[j++] = toupper(c);
    }
    
    // Skip to extension part
    while (input[i] && input[i] != '.') {
        i++;
    }
    
    // Process the extension part (up to 3 characters)
    if (input[i] == '.') {
        i++;
        for (j = 8; input[i] && j < 11; i++) {
            char c = input[i];
            if (c == ' ' || c == '.' || c == '/' || c == '\\' || c == ':' || 
                c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || 
                c == '|' || c < ' ') {
                continue; // Skip invalid characters
            }
            output[j++] = toupper(c);
        }
    }
    
    // Ensure null-termination if output is treated as a string
    output[11] = '\0';
}