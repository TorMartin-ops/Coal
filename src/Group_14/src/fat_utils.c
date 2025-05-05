/**
 * @file fat_utils.c
 * @brief Utility functions for FAT filesystem driver.
 */

// --- Includes ---
#include "fat_utils.h"  // Header for this file's declarations
#include "fat_core.h"   // For fat_fs_t definition, logging macros if defined here
#include "fat_fs.h"     // For fat_dir_entry_t, FAT_DIR_ENTRY_UNUSED/DELETED macros etc.
#include "fat_dir.h"    // NEEDED for read_directory_sector declaration (if not moved to fat_utils.h)
#include "string.h"     // For strlen, strcmp, memset, memcpy
#include "libc/ctype.h" // For toupper
#include "libc/stdio.h" // For sprintf (if used by itoa)
#include "fs_errno.h"   // Filesystem error codes
#include "terminal.h"   // Logging (terminal_printf)
#include "assert.h"     // KERNEL_ASSERT
#include "kmalloc.h"    // *** ADDED for kmalloc/kfree ***
#include <libc/stdbool.h> // For bool (assuming path)
#include <libc/stdint.h>  // For uint*_t (assuming path)

// --- Logging Macros (Ensure defined correctly) ---
// Assuming they are defined elsewhere (e.g., fat_core.h or debug.h)
// If not, copy the definitions from previous examples here.
#ifndef FAT_DEBUG_LOG
#define FAT_DEBUG_LOG(fmt, ...) do {} while(0)
#endif
#ifndef FAT_INFO_LOG
#define FAT_INFO_LOG(fmt, ...)  terminal_printf("[fat_utils:INFO] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef FAT_WARN_LOG
#define FAT_WARN_LOG(fmt, ...)  terminal_printf("[fat_utils:WARN] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef FAT_ERROR_LOG
#define FAT_ERROR_LOG(fmt, ...) terminal_printf("[fat_utils:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#endif

/* --- Static Helper Prototypes --- */
static int _itoa_simple(int value, char* buf, int max_len);


/**
 * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
 */
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster)
{
    KERNEL_ASSERT(fs != NULL, "NULL fs context in fat_cluster_to_lba");
    if (cluster < 2) return 0; // Clusters 0 and 1 are reserved/invalid for data
    // Calculation assumes fs->first_data_sector and fs->sectors_per_cluster are valid
    return fs->first_data_sector + ((cluster - 2) * fs->sectors_per_cluster);
}

/**
 * @brief Retrieves the next cluster in the chain from the FAT table.
 */
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster)
{
    KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL && next_cluster != NULL, "Invalid arguments to fat_get_next_cluster");

    // Calculate max valid cluster index based on FAT size and type
    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) return -FS_ERR_INVALID_FORMAT; // Includes FAT12 currently
    // Max index is one less than the total number of entries possible
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1;

    // Check bounds (cluster numbers are 2 to max_cluster_index+1 typically)
    // The highest cluster number usable is total_data_clusters + 1
    if (current_cluster < 2 || current_cluster > (fs->total_data_clusters + 1)) {
         terminal_printf("[FAT Get Next] Warning: Current cluster %lu potentially out of valid data range (2-%lu).\n",
                         (unsigned long)current_cluster, (unsigned long)fs->total_data_clusters + 1);
         // Allow reading FAT entry even slightly out of data range, but check against FAT table size
    }
     if (current_cluster > max_cluster_index) {
         terminal_printf("[FAT Get Next] Error: Current cluster %lu out of FAT table bounds (max index %lu).\n",
                         (unsigned long)current_cluster, (unsigned long)max_cluster_index);
         return -FS_ERR_INVALID_PARAM;
     }


    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        *next_cluster = FAT32[current_cluster] & 0x0FFFFFFF; // Mask out reserved bits
    } else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        *next_cluster = FAT16[current_cluster];
    } else { // FAT12 - Not fully supported here
         FAT_ERROR_LOG("fat_get_next_cluster: FAT12 not implemented.");
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
    if (entry_size == 0) return -FS_ERR_INVALID_FORMAT; // Includes FAT12
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1; // Max valid index

     // Check bounds
     if (cluster > max_cluster_index) {
         terminal_printf("[FAT Get Entry] Error: Cluster index %lu out of FAT table bounds (max index %lu).\n",
                         (unsigned long)cluster, (unsigned long)max_cluster_index);
         return -FS_ERR_INVALID_PARAM;
     }


    switch (fs->type) {
        case FAT_TYPE_FAT16:
            fat_offset = cluster * 2;
            // Check if offset itself is within bounds (already covered by cluster check)
            *entry_value = (uint32_t)(*(uint16_t*)((uint8_t*)fs->fat_table + fat_offset));
            break;
        case FAT_TYPE_FAT32:
            fat_offset = cluster * 4;
            // Check if offset itself is within bounds (already covered by cluster check)
            *entry_value = (*(uint32_t*)((uint8_t*)fs->fat_table + fat_offset)) & 0x0FFFFFFF; // Mask reserved bits
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
      if (entry_size == 0) return -FS_ERR_INVALID_FORMAT; // Includes FAT12
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1; // Max valid index

     // Check bounds
     if (cluster < 2 || cluster > max_cluster_index) { // Cannot set entries 0 or 1 normally
         terminal_printf("[FAT Set Entry] Error: Cluster index %lu out of valid range (2-%lu).\n",
                         (unsigned long)cluster, (unsigned long)max_cluster_index);
         return -FS_ERR_INVALID_PARAM;
     }


    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        // Preserve reserved bits (top 4 bits) when writing
        FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } else if (fs->type == FAT_TYPE_FAT16) {
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        FAT16[cluster] = (uint16_t)value;
    } else { // FAT12
         FAT_ERROR_LOG("fat_set_cluster_entry: FAT12 not implemented.");
        return -FS_ERR_NOT_SUPPORTED;
    }

    fs->fat_dirty = true; // Mark FAT as modified
    return FS_SUCCESS;
}

/**
 * @brief Converts a standard filename to FAT 8.3 format.
 */
void format_filename(const char *input, char output_8_3[11])
{
    size_t i, j;
    // Start with spaces
    memset(output_8_3, ' ', 11);
    if (!input) return;

    // Skip leading dots and spaces
    while (*input && (*input == '.' || *input == ' ')) { input++; }
    if (!*input) { // If name is empty after skipping
        memcpy(output_8_3, "EMPTY   ", 8); // Use a default name or handle error?
        return;
    }

    // Process base name part (up to 8 chars, before '.')
    for (i = 0, j = 0; input[i] && input[i] != '.' && j < 8; i++) {
        char c = input[i];
        // Handle special first byte case for E5 -> 05
        if (j == 0 && (unsigned char)c == 0xE5) {
            output_8_3[j++] = 0x05;
        } else if (c == ' ') { // Skip embedded spaces
            continue;
        } else if (c < ' ' || strchr("\"*+,./:;<=>?[\\]|", c)) { // Check invalid chars (period added)
             output_8_3[j++] = '_'; // Replace invalid with underscore
        } else {
             output_8_3[j++] = toupper((unsigned char)c); // Convert valid chars to uppercase
        }
    }

    // Find the start of the extension (skip remaining base chars if > 8)
    while (input[i] && input[i] != '.') { i++; }

    // Process extension part (up to 3 chars, after '.')
    if (input[i] == '.') {
        i++; // Move past the dot
        for (j = 8; input[i] && j < 11; i++) { // Start filling at index 8
            char c = input[i];
            if (c == ' ' || c == '.') { // Skip spaces and subsequent dots in extension
                 continue;
            } else if (c < ' ' || strchr("\"*+,/:;<=>?[\\]|", c)) {
                 output_8_3[j++] = '_';
            } else {
                 output_8_3[j++] = toupper((unsigned char)c);
            }
        }
    }
    // The rest of output_8_3 remains padded with spaces from the initial memset.
}

/**
 * @brief Compares a filename component string against a reconstructed LFN string (case-insensitive).
 */
int fat_compare_lfn(const char* component, const char* reconstructed_lfn) {
    // Basic NULL checks
    if (!component && !reconstructed_lfn) return 0;
    if (!component) return -1; // NULL < non-NULL
    if (!reconstructed_lfn) return 1; // non-NULL > NULL

    // Case-insensitive comparison
    while (*component && *reconstructed_lfn) {
        int diff = toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
        if (diff != 0) {
            return diff; // Return difference at first non-matching char
        }
        component++;
        reconstructed_lfn++;
    }
    // Return difference based on which string ended first (or if both ended)
    return toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
}

/**
 * @brief Compares a filename component string against a raw 8.3 FAT filename (case-insensitive).
 */
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]) {
    if (!component || !name_8_3) return -1; // Handle NULL

    char formatted_component_83[11];
    format_filename(component, formatted_component_83);

    // Perform binary comparison of the formatted name and the raw directory entry name
    return memcmp(formatted_component_83, name_8_3, 11);
}

// --- Short Name Generation & Collision Check Implementation ---

/**
 * @brief Checks if a directory entry with the exact raw 11-byte short name exists.
 */
bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]) {
    KERNEL_ASSERT(fs != NULL && short_name_raw != NULL, "NULL fs or name pointer");
    // Assumes caller holds fs->lock if concurrent modification is possible

    uint32_t current_cluster = dir_cluster;
    bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
    uint32_t current_byte_offset = 0;
    bool found = false;
    int io_error = FS_SUCCESS; // Track I/O errors during scan

    uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
    if (!sector_data) {
        FAT_ERROR_LOG("fat_raw_short_name_exists: Failed to allocate sector buffer.");
        return true; // Fail safe: Assume it exists if we can't check due to OOM.
    }

    while (true) {
        if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break;

        uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);

        // Ensure read_directory_sector is declared (e.g., in fat_dir.h or fat_io.h)
        int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
        if (read_res != FS_SUCCESS) {
            io_error = read_res;
            break;
        }

        for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
            fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));

            // Use macros defined in fat_fs.h
            if (de->name[0] == FAT_DIR_ENTRY_UNUSED) { goto check_done; }
            if (de->name[0] == FAT_DIR_ENTRY_DELETED) continue;
            if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) continue;
            if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) continue;

            if (memcmp(de->name, short_name_raw, 11) == 0) {
                found = true;
                goto check_done;
            }
        }

        current_byte_offset += fs->bytes_per_sector;

        if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) { io_error = get_next_res; break; }
            if (next_c >= fs->eoc_marker) break;
            current_cluster = next_c;
            current_byte_offset = 0;
        }
        if (scanning_fixed_root && current_byte_offset >= (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) {
            break;
        }
    }

check_done:
    kfree(sector_data);
    if (io_error != FS_SUCCESS) {
        FAT_ERROR_LOG("fat_raw_short_name_exists: I/O error %d during check.", io_error);
        return true; // Fail safe
    }
    return found;
}


/**
 * @brief Gets the current time and date in FAT timestamp format. (FIXED TIME)
 */
void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date)
{
    KERNEL_ASSERT(fat_time != NULL && fat_date != NULL, "NULL output pointer in fat_get_current_timestamp");

    // Using a fixed date/time for now. Replace with RTC or PIT logic later.
    uint16_t year = 2025; uint16_t month = 5; uint16_t day = 5;
    uint16_t hours = 11; uint16_t minutes = 45; uint16_t seconds = 0; // Updated time slightly

    if (year < 1980) year = 1980; // FAT epoch start

    *fat_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *fat_time = (uint16_t)((hours << 11) | (minutes << 5) | (seconds >> 1));

    FAT_DEBUG_LOG("Returning fixed timestamp: Date=0x%04x, Time=0x%04x", *fat_date, *fat_time);
}


// --- Static Helper Implementation for Short Name Generation ---
/**
 * @brief Simple integer to ASCII conversion helper.
 */
static int _itoa_simple(int value, char* buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    if (value == 0) {
        if (max_len < 2) return -1;
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    if (value < 0) return -1; // Not handled

    int i = 0;
    int temp_val = value;

    // Calculate digits needed (and store in reverse order temporarily)
    while (temp_val > 0 && i < max_len - 1) {
        buf[i++] = (temp_val % 10) + '0';
        temp_val /= 10;
    }

    if (temp_val > 0) { return -1; } // Buffer wasn't long enough

    buf[i] = '\0';
    int len = i;

    // Reverse the string in place
    int start = 0;
    int end = len - 1;
    while (start < end) {
        char temp = buf[start];
        buf[start] = buf[end];
        buf[end] = temp;
        start++;
        end--;
    }
    return len;
}

/**
 * @brief Generates a unique 8.3 short filename based on a long filename.
 */
int fat_generate_short_name(fat_fs_t *fs,
                             uint32_t parent_dir_cluster,
                             const char* long_name,
                             uint8_t short_name_out[11])
{
    KERNEL_ASSERT(fs != NULL && long_name != NULL && short_name_out != NULL,
                  "FS context, long name, and output buffer cannot be NULL for short name generation");

    char base_name_char[11];
    uint8_t trial_name[11];
    char num_suffix[8]; // ~ + 6 digits + null

    // 1. Generate initial base 8.3 name
    format_filename(long_name, base_name_char);

    // 2. Check if base name is unique
    if (!fat_raw_short_name_exists(fs, parent_dir_cluster, (uint8_t*)base_name_char)) {
        memcpy(short_name_out, base_name_char, 11);
        FAT_DEBUG_LOG("Base name '%.11s' is unique for '%s'.", base_name_char, long_name);
        return FS_SUCCESS;
    }

    FAT_DEBUG_LOG("Base name '%.11s' collides for '%s'. Generating ~N suffix...", base_name_char, long_name);

    // 3. Generate ~N variations
    for (int n = 1; n <= 999999; ++n) {
        num_suffix[0] = '~';
        int num_len = _itoa_simple(n, num_suffix + 1, sizeof(num_suffix) - 1);
        if (num_len < 0) {
            FAT_ERROR_LOG("Failed to convert suffix number %d to string.", n);
            return -FS_ERR_INTERNAL;
        }
        int suffix_len = 1 + num_len;

        int base_chars_to_keep = 8 - suffix_len;
        if (base_chars_to_keep < 1) base_chars_to_keep = 1; // Should keep at least 1 char

        // Construct trial name
        memset(trial_name, ' ', 11);
        memcpy(trial_name, base_name_char, base_chars_to_keep);
        memcpy(trial_name + base_chars_to_keep, num_suffix, suffix_len);
        memcpy(trial_name + 8, base_name_char + 8, 3); // Copy original extension

        // Check uniqueness
        if (!fat_raw_short_name_exists(fs, parent_dir_cluster, trial_name)) {
            memcpy(short_name_out, trial_name, 11);
            FAT_DEBUG_LOG("Unique name '%.11s' found for '%s'.", trial_name, long_name);
            return FS_SUCCESS;
        }
    }

    FAT_ERROR_LOG("Could not generate unique short name for '%s' after %d attempts.", long_name, 999999);
    return -FS_ERR_NAMETOOLONG; // Or appropriate error like FS_ERR_EXISTS
}


// --- End of file fat_utils.c ---