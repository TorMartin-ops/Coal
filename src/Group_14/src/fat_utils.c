/**
 * @file fat_utils.c
 * @brief Utility functions for FAT filesystem driver.
 */

// --- Includes ---
#include "fat_utils.h"   // Header for this file's declarations
#include "fat_core.h"    // For fat_fs_t definition
#include "fat_fs.h"      // For fat_dir_entry_t definition
#include "string.h"      // For strlen, strcmp, memset, memcpy
#include "libc/ctype.h"  // For toupper
#include "libc/stdio.h"  // For sprintf (if used)
#include "fs_errno.h"    // Filesystem error codes
#include "terminal.h"    // Logging (terminal_printf)
#include "assert.h"      // KERNEL_ASSERT
#include "libc/stdbool.h"// For bool
#include "libc/stdint.h" // For uint*_t types

// --- Logging Macros (Ensure defined correctly, e.g., in fat_core.h or similar) ---
#ifndef FAT_DEBUG_LOG
#ifdef KLOG_LEVEL_DEBUG
#define FAT_DEBUG_LOG(fmt, ...) terminal_printf("[fat_utils:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_DEBUG_LOG(fmt, ...) do {} while(0)
#endif
#endif
#ifndef FAT_INFO_LOG
#define FAT_INFO_LOG(fmt, ...)  terminal_printf("[fat_utils:INFO]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#endif
#ifndef FAT_WARN_LOG
#define FAT_WARN_LOG(fmt, ...)  terminal_printf("[fat_utils:WARN]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#endif
#ifndef FAT_ERROR_LOG
#define FAT_ERROR_LOG(fmt, ...) terminal_printf("[fat_utils:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#endif
// --- End Logging Macros ---


/**
 * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
 */
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster)
{
    KERNEL_ASSERT(fs != NULL, "NULL fs context in fat_cluster_to_lba");
    if (cluster < 2) return 0;
    return fs->first_data_sector + ((cluster - 2) * fs->sectors_per_cluster);
}

/**
 * @brief Retrieves the next cluster in the chain from the FAT table.
 */
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster)
{
    KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL && next_cluster != NULL, "Invalid arguments to fat_get_next_cluster");

    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) return -FS_ERR_INVALID_FORMAT;
    uint32_t max_cluster_index = fat_size_bytes / entry_size;

    if (current_cluster >= max_cluster_index) {
        // Corrected format specifiers
        terminal_printf("[FAT Get Next] Error: Current cluster %lu out of bounds (max %lu).\n",
                        (unsigned long)current_cluster, (unsigned long)max_cluster_index);
        return -FS_ERR_INVALID_PARAM;
    }

    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        *next_cluster = FAT32[current_cluster] & 0x0FFFFFFF;
    } else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        *next_cluster = FAT16[current_cluster];
    } else {
        return -FS_ERR_NOT_SUPPORTED;
    }
    return FS_SUCCESS;
}

/**
 * @brief Retrieves the value of a specific cluster entry from the FAT table.
 */
int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value)
{
    KERNEL_ASSERT(fs != NULL && entry_value != NULL, "NULL fs or entry_value pointer");
    KERNEL_ASSERT(fs->fat_table != NULL, "FAT table not loaded");

    size_t fat_offset;
    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) return -FS_ERR_INVALID_FORMAT;
    uint32_t max_cluster_index = fat_size_bytes / entry_size;

    if (cluster >= max_cluster_index) {
        // Corrected format specifiers
        terminal_printf("[FAT Get Entry] Error: Cluster index %lu out of bounds (max %lu).\n",
                        (unsigned long)cluster, (unsigned long)max_cluster_index);
        return -FS_ERR_INVALID_PARAM;
    }

    switch (fs->type) {
        case FAT_TYPE_FAT16:
            fat_offset = cluster * 2;
            if (fat_offset + 1 >= fat_size_bytes) {
                // Corrected format specifiers
                terminal_printf("[FAT Get Entry FAT16] Error: Calculated offset %lu out of bounds (%lu).\n",
                                (unsigned long)fat_offset, (unsigned long)fat_size_bytes);
                return -FS_ERR_INTERNAL;
            }
            *entry_value = (uint32_t)(*(uint16_t*)((uint8_t*)fs->fat_table + fat_offset));
            break;
        case FAT_TYPE_FAT32:
            fat_offset = cluster * 4;
            if (fat_offset + 3 >= fat_size_bytes) {
                // Corrected format specifiers
                terminal_printf("[FAT Get Entry FAT32] Error: Calculated offset %lu out of bounds (%lu).\n",
                                (unsigned long)fat_offset, (unsigned long)fat_size_bytes);
                return -FS_ERR_INTERNAL;
            }
            *entry_value = (*(uint32_t*)((uint8_t*)fs->fat_table + fat_offset)) & 0x0FFFFFFF;
            break;
        case FAT_TYPE_FAT12:
            FAT_ERROR_LOG("FAT12 get_cluster_entry not fully implemented.");
            return -FS_ERR_NOT_SUPPORTED;
        default:
            return -FS_ERR_INVALID_FORMAT;
    }
    return FS_SUCCESS;
}

/**
 * @brief Updates the FAT entry for a given cluster with a new value.
 */
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value)
{
    KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL, "Invalid arguments to fat_set_cluster_entry");

    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
     if (entry_size == 0) return -FS_ERR_INVALID_FORMAT;
    uint32_t max_cluster_index = fat_size_bytes / entry_size;

    if (cluster >= max_cluster_index) {
        // Corrected format specifiers
        terminal_printf("[FAT Set Entry] Error: Cluster index %lu out of bounds (max %lu).\n",
                        (unsigned long)cluster, (unsigned long)max_cluster_index);
        return -FS_ERR_INVALID_PARAM;
    }

    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        FAT16[cluster] = (uint16_t)value;
    } else {
        return -FS_ERR_NOT_SUPPORTED;
    }
    // TODO: Mark FAT table dirty
    return FS_SUCCESS;
}

/**
 * @brief Converts a standard filename to FAT 8.3 format.
 * @param input The null-terminated input filename.
 * @param output_8_3 A character array of exactly 11 bytes (NOT null terminated).
 * Corrected signature to match header: takes char* for output.
 */
void format_filename(const char *input, char output_8_3[11])
{
    size_t i, j;
    memset(output_8_3, ' ', 11);
    if (!input) return;
    while (*input && (*input == '.' || *input == ' ')) { input++; }
    if (!*input) { memcpy(output_8_3, "EMPTY   ", 8); return; }

    for (i = 0, j = 0; input[i] && input[i] != '.' && j < 8; i++) {
        char c = input[i];
        if (c < ' ' || strchr("\"*+,/:;<=>?[\\]|.", c)) { output_8_3[j++] = '_'; }
        else { output_8_3[j++] = toupper((unsigned char)c); }
    }
    while (input[i] && input[i] != '.') { i++; }
    if (input[i] == '.') {
        i++;
        for (j = 8; input[i] && j < 11; i++) {
            char c = input[i];
            if (c < ' ' || strchr("\"*+,/:;<=>?[\\]|.", c)) { output_8_3[j++] = '_'; }
            else { output_8_3[j++] = toupper((unsigned char)c); }
        }
    }
}

/**
 * @brief Compares a filename component string against a reconstructed LFN string (case-insensitive).
 */
int fat_compare_lfn(const char* component, const char* reconstructed_lfn) {
    if (!component || !reconstructed_lfn) return -1;
    while (*component && *reconstructed_lfn) {
        if (toupper((unsigned char)*component) != toupper((unsigned char)*reconstructed_lfn)) {
            return toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
        }
        component++; reconstructed_lfn++;
    }
    return toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
}

/**
 * @brief Compares a filename component string against a raw 8.3 FAT filename (case-insensitive).
 */
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]) {
    if (!component || !name_8_3) return -1;
    char formatted_component_83[11]; // Use char array to match format_filename output type
    format_filename(component, formatted_component_83); // Use the correct function name and output type
    // Compare using memcmp (binary comparison)
    return memcmp(formatted_component_83, name_8_3, 11);
}

// --- NEW Function Implementations ---

/**
 * @brief Generates a unique FAT 8.3 short filename. (Simplified version)
 */
int fat_generate_short_name(fat_fs_t *fs, uint32_t parent_dir_cluster, const char *long_name, uint8_t short_name_raw[11])
{
    KERNEL_ASSERT(fs != NULL && long_name != NULL && short_name_raw != NULL, "NULL parameter in fat_generate_short_name");
    // Assumes caller holds fs lock

    // 1. Generate the base 8.3 name using format_filename
    // NOTE: format_filename outputs char[11], but short_name_raw is uint8_t[11].
    //       Casting is okay here as we treat it as raw bytes.
    format_filename(long_name, (char*)short_name_raw); // Cast output buffer type

    // 2. Check if the base name already exists
    if (!fat_raw_short_name_exists(fs, parent_dir_cluster, short_name_raw)) {
        FAT_DEBUG_LOG("Generated unique 8.3 name '%.11s' directly.", short_name_raw);
        return FS_SUCCESS;
    }

    // 3. Base name exists, try adding "~1"
    FAT_DEBUG_LOG("Base 8.3 name '%.11s' exists, trying ~1.", short_name_raw);
    int base_len = 8;
    while (base_len > 0 && short_name_raw[base_len - 1] == ' ') { base_len--; }
    if (base_len > 6) { base_len = 6; }
    short_name_raw[base_len] = '~';
    short_name_raw[base_len + 1] = '1';
    for(int i = base_len + 2; i < 8; ++i) { short_name_raw[i] = ' '; }

    // Check if the "~1" name exists
    if (!fat_raw_short_name_exists(fs, parent_dir_cluster, short_name_raw)) {
        FAT_DEBUG_LOG("Generated unique 8.3 name '%.11s' using ~1.", short_name_raw);
        return FS_SUCCESS;
    }

    FAT_ERROR_LOG("Failed to generate unique 8.3 name for '%s' - Both base and '~1' names exist.", long_name);
    // Use a defined error code from fs_errno.h. Check fs_errno.h for FS_ERR_ALREADY_EXISTS.
    // Using FS_ERR_UNKNOWN as fallback if specific code unavailable.
    #ifdef FS_ERR_ALREADY_EXISTS
        return -FS_ERR_ALREADY_EXISTS;
    #else
        return -FS_ERR_UNKNOWN; // Or -FS_ERR_NO_SPACE
    #endif
}

/**
 * @brief Gets the current time and date in FAT timestamp format. (FIXED TIME)
 */
void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date)
{
    KERNEL_ASSERT(fat_time != NULL && fat_date != NULL, "NULL output pointer in fat_get_current_timestamp");

    // Using April 18, 2025, 15:21:11 (Based on conversation context time)
    uint16_t year = 2025; uint16_t month = 4; uint16_t day = 18;
    uint16_t hours = 15; uint16_t minutes = 21; uint16_t seconds = 11;

    if (year < 1980) year = 1980;

    *fat_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *fat_time = (uint16_t)((hours << 11) | (minutes << 5) | (seconds >> 1));

    FAT_DEBUG_LOG("Returning fixed timestamp: Date=0x%04x, Time=0x%04x", *fat_date, *fat_time);
}

// --- End of fat_utils.c ---