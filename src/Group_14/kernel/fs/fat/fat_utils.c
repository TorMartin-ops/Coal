/**
 * @file fat_utils.c
 * @brief Utility functions for FAT filesystem driver (v1.1 - Corrected Implementations)
 *
 * Implements helper functions for cluster/LBA conversion, FAT table access,
 * filename formatting/comparison, timestamp retrieval, and short name generation.
 */

// --- Includes ---
#include <kernel/fs/fat/fat_utils.h>  // Header for this file's declarations
#include <kernel/fs/fat/fat_core.h>   // For fat_fs_t definition, logging macros if defined here
#include <kernel/fs/fat/fat_fs.h>     // For fat_dir_entry_t, FAT_DIR_ENTRY_UNUSED/DELETED macros etc.
#include <kernel/fs/fat/fat_dir.h>    // NEEDED for read_directory_sector declaration
#include <kernel/lib/string.h>     // For strlen, strcmp, memset, memcpy, strchr, strrchr
#include <libc/ctype.h> // For toupper
#include <libc/stdio.h> // For sprintf/snprintf (if used by itoa)
#include <kernel/fs/vfs/fs_errno.h>   // Filesystem error codes
#include <kernel/drivers/display/terminal.h>   // Logging (terminal_printf)
#include <kernel/lib/assert.h>     // KERNEL_ASSERT
#include <kernel/memory/kmalloc.h>    // For kmalloc/kfree used in helpers
#include <libc/stdbool.h> // For bool
#include <libc/stdint.h>  // For uint*_t
#include <libc/stddef.h>  // For size_t, NULL
#include <kernel/drivers/timer/time.h>       // For kernel_time_t and kernel_get_time (if available)

// --- Logging Macros (Ensure defined correctly) ---
// Define FAT_DEBUG_LEVEL to 1 or higher for debug logs
#ifndef FAT_DEBUG_LEVEL
#define FAT_DEBUG_LEVEL 0
#endif

#if FAT_DEBUG_LEVEL >= 1
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[fat_utils:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_DEBUG_LOG(fmt, ...) do {} while(0)
#endif
#define FAT_INFO_LOG(fmt, ...)  terminal_printf("[fat_utils:INFO]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_WARN_LOG(fmt, ...)  terminal_printf("[fat_utils:WARN]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) terminal_printf("[fat_utils:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
// --- End Logging Macros ---

/* --- Static Helper Prototypes --- */
static int _itoa_simple(int value, char* buf, int max_len);

/* --- FAT Geometry and FAT Table Access --- */

/**
 * @brief Converts a FAT cluster number into its corresponding Logical Block Address (LBA).
 */
uint32_t fat_cluster_to_lba(fat_fs_t *fs, uint32_t cluster) {
    KERNEL_ASSERT(fs != NULL, "NULL fs context in fat_cluster_to_lba");
    KERNEL_ASSERT(fs->sectors_per_cluster > 0, "Sectors per cluster is zero");
    if (cluster < 2) {
        FAT_WARN_LOG("Attempted to get LBA for reserved cluster %lu", (unsigned long)cluster);
        return 0; // Clusters 0 and 1 are reserved/invalid for data
    }
    // Calculation assumes fs->first_data_sector and fs->sectors_per_cluster are valid
    // Ensure no overflow in intermediate calculation, although unlikely with typical FAT values
    uint64_t offset = (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    uint64_t lba = (uint64_t)fs->first_data_sector + offset;

    if (lba > 0xFFFFFFFF) { // Check if LBA exceeds 32-bit limit (relevant for large disks)
        FAT_WARN_LOG("Calculated LBA %llu exceeds 32-bit limit for cluster %lu", lba, (unsigned long)cluster);
        // Handle error appropriately - perhaps return 0 or a specific error code?
        // For now, return 0 as it indicates an invalid LBA in this context.
        return 0;
    }
    return (uint32_t)lba;
}

/**
 * @brief Retrieves the next cluster in the chain from the FAT table.
 */
int fat_get_next_cluster(fat_fs_t *fs, uint32_t current_cluster, uint32_t *next_cluster) {
    KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL && next_cluster != NULL, "Invalid arguments to fat_get_next_cluster");

    // Calculate max valid cluster index based on FAT size and type
    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) {
        FAT_ERROR_LOG("FAT12 not supported in fat_get_next_cluster");
        return FS_ERR_NOT_SUPPORTED;
    }
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1;

    // Check bounds more strictly - must be within the actual data cluster range OR valid reserved values
    if (current_cluster < 2 || current_cluster > (fs->total_data_clusters + 1)) {
        FAT_WARN_LOG("Current cluster %lu out of valid data range (2-%lu).",
                     (unsigned long)current_cluster, (unsigned long)fs->total_data_clusters + 1);
        // Allow reading FAT entry for clusters slightly out of data range (e.g., EOC markers),
        // but ensure it's within the FAT table bounds itself.
        if (current_cluster > max_cluster_index) {
            FAT_ERROR_LOG("Current cluster %lu out of FAT table bounds (max index %lu).",
                          (unsigned long)current_cluster, (unsigned long)max_cluster_index);
            return FS_ERR_INVALID_PARAM;
        }
    }

    // Read the entry based on FAT type
    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        *next_cluster = FAT32[current_cluster] & 0x0FFFFFFF; // Mask out reserved bits
    } else { // FAT_TYPE_FAT16
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        *next_cluster = FAT16[current_cluster];
    }
    return FS_SUCCESS;
}

/**
 * @brief Retrieves the value of a specific cluster entry from the FAT table.
 */
int fat_get_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t *entry_value) {
    KERNEL_ASSERT(fs != NULL && entry_value != NULL, "NULL fs or entry_value pointer");
    KERNEL_ASSERT(fs->fat_table != NULL, "FAT table not loaded");

    size_t fat_offset;
    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) {
        FAT_ERROR_LOG("FAT12 get_cluster_entry not fully implemented.");
        return FS_ERR_NOT_SUPPORTED;
    }
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1; // Max valid index

    // Check bounds
    if (cluster > max_cluster_index) {
        FAT_ERROR_LOG("Cluster index %lu out of FAT table bounds (max index %lu).",
                      (unsigned long)cluster, (unsigned long)max_cluster_index);
        return FS_ERR_INVALID_PARAM;
    }

    switch (fs->type) {
        case FAT_TYPE_FAT16:
            fat_offset = cluster * 2;
            *entry_value = (uint32_t)(*(uint16_t*)((uint8_t*)fs->fat_table + fat_offset));
            break;
        case FAT_TYPE_FAT32:
            fat_offset = cluster * 4;
            *entry_value = (*(uint32_t*)((uint8_t*)fs->fat_table + fat_offset)) & 0x0FFFFFFF; // Mask reserved bits
            break;
        default: // Includes FAT12
            FAT_ERROR_LOG("Unsupported FAT type %d in get_cluster_entry", fs->type);
            return FS_ERR_INVALID_FORMAT;
    }
    return FS_SUCCESS;
}

/**
 * @brief Updates the FAT entry for a given cluster with a new value.
 */
int fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    KERNEL_ASSERT(fs != NULL && fs->fat_table != NULL, "Invalid arguments to fat_set_cluster_entry");

    size_t fat_size_bytes = (size_t)fs->fat_size_sectors * fs->bytes_per_sector;
    size_t entry_size = (fs->type == FAT_TYPE_FAT16 ? 2 : (fs->type == FAT_TYPE_FAT32 ? 4 : 0));
    if (entry_size == 0) {
        FAT_ERROR_LOG("FAT12 set_cluster_entry not implemented.");
        return FS_ERR_NOT_SUPPORTED;
    }
    uint32_t max_cluster_index = (fat_size_bytes / entry_size) - 1;

    // Check bounds - Allow setting entries 0 and 1? Usually not needed by driver.
    // Let's restrict to valid data cluster range for safety.
    if (cluster < 2 || cluster > max_cluster_index) {
        FAT_ERROR_LOG("Cluster index %lu out of valid range (2-%lu) for set_cluster_entry.",
                      (unsigned long)cluster, (unsigned long)max_cluster_index);
        return FS_ERR_INVALID_PARAM;
    }

    // Modify the in-memory FAT table
    if (fs->type == FAT_TYPE_FAT32) {
        uint32_t *FAT32 = (uint32_t*)fs->fat_table;
        // Preserve reserved bits (top 4 bits) when writing
        FAT32[cluster] = (FAT32[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    } else { // FAT_TYPE_FAT16
        uint16_t *FAT16 = (uint16_t*)fs->fat_table;
        FAT16[cluster] = (uint16_t)value;
    }

    fs->fat_dirty = true; // Mark FAT as modified, needs flushing later
    return FS_SUCCESS;
}

/* --- Filename Formatting and Comparison --- */

/**
 * @brief Converts a standard filename string into the 11-byte FAT 8.3 format.
 */
void format_filename(const char *input, char output_8_3[11]) {
    size_t i, j;
    memset(output_8_3, ' ', 11); // Initialize with spaces
    if (!input) return;

    // Skip leading dots and spaces (common in user input)
    while (*input && (*input == '.' || *input == ' ')) { input++; }
    if (!*input) {
        memcpy(output_8_3, "EMPTY   ", 8); // Handle empty name case
        return;
    }

    // Process base name part
    for (i = 0, j = 0; input[i] && input[i] != '.' && j < 8; i++) {
        char c = input[i];
        if (j == 0 && (unsigned char)c == 0xE5) { // Handle KANJI replacement
            output_8_3[j++] = 0x05;
        } else if (c == ' ') { // Skip embedded spaces
            continue;
        } else if (c < ' ' || strchr("\"*+,./:;<=>?[\\]|", c)) { // Replace invalid chars
            output_8_3[j++] = '_';
        } else {
            output_8_3[j++] = toupper((unsigned char)c); // Uppercase and copy
        }
    }

    // Find the start of the extension
    while (input[i] && input[i] != '.') { i++; }

    // Process extension part
    if (input[i] == '.') {
        i++; // Move past the dot
        for (j = 8; input[i] && j < 11; i++) { // Start filling at index 8
            char c = input[i];
            if (c == ' ' || c == '.') { continue; } // Skip spaces/dots in extension
            else if (c < ' ' || strchr("\"*+,/:;<=>?[\\]|", c)) {
                output_8_3[j++] = '_';
            } else {
                output_8_3[j++] = toupper((unsigned char)c);
            }
        }
    }
    // Remainder is space-padded from initial memset
}

/**
 * @brief Compares a filename component string against a reconstructed LFN string (case-insensitive).
 */
int fat_compare_lfn(const char* component, const char* reconstructed_lfn) {
    if (!component && !reconstructed_lfn) return 0;
    if (!component) return -1;
    if (!reconstructed_lfn) return 1;

    while (*component && *reconstructed_lfn) {
        int diff = toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
        if (diff != 0) return diff;
        component++;
        reconstructed_lfn++;
    }
    return toupper((unsigned char)*component) - toupper((unsigned char)*reconstructed_lfn);
}

/**
 * @brief Compares a filename component string against a raw 11-byte 8.3 FAT filename (case-insensitive).
 */
int fat_compare_8_3(const char* component, const uint8_t name_8_3[11]) {
    if (!component || !name_8_3) return -1;
    char formatted_component_83[11];
    format_filename(component, formatted_component_83);
    return memcmp(formatted_component_83, name_8_3, 11);
}

/* --- Short Name Generation & Collision Check --- */

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
    int io_error = FS_SUCCESS;

    uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
    if (!sector_data) {
        FAT_ERROR_LOG("Failed to allocate sector buffer.");
        return true; // Fail safe: Assume it exists if we can't check.
    }

    while (true) {
        if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break;
        if (scanning_fixed_root && current_byte_offset >= (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) break;

        uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);

        int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
        if (read_res != FS_SUCCESS) { io_error = read_res; break; }

        for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
            fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
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
    }

check_done:
    kfree(sector_data);
    if (io_error != FS_SUCCESS) {
        FAT_ERROR_LOG("I/O error %d during short name check.", io_error);
        return true; // Fail safe
    }
    return found;
}

/**
 * @brief Generates a unique 8.3 short filename based on a long filename.
 * Implements the standard "NAME~N.EXT" generation algorithm.
 * @note Assumes caller holds fs->lock.
 */
int fat_generate_short_name(fat_fs_t   *fs,
                            uint32_t    parent_dir_cluster, // <-- Added parameter
                            const char *long_name,
                            uint8_t     short_name_out[11])
{
    KERNEL_ASSERT(fs && long_name && short_name_out, "Invalid args to fat_generate_short_name");
    if (!*long_name) return FS_ERR_INVALID_PARAM; // Empty long name

    // 0. Normalise and split at the last '.'
    char base[9] = { ' ' }; // Initialize with spaces
    char ext[4] = { ' ' };  // Initialize with spaces
    const char *dot = strrchr(long_name, '.');
    size_t len_base = dot ? (size_t)(dot - long_name) : strlen(long_name);
    size_t len_ext  = dot ? strlen(dot + 1) : 0;

    // 1. Copy and sanitize base name (up to 8 chars)
    size_t i, j = 0;
    for (i = 0; i < len_base && j < 8; ++i) {
        char c = toupper((unsigned char)long_name[i]);
        // Replace invalid characters or skip spaces/dots
        if (c == '.' || c == ' ') continue; // Skip dots/spaces within base
        if (strchr("\"*+,/:;<=>?[\\]|", c) || c < 0x20) c = '_';
        // Handle KANJI E5 replacement at the start
        if (j == 0 && c == (char)0xE5) c = 0x05;
        base[j++] = c;
    }
    // Pad remaining base with spaces if needed (already done by init)

    // 2. Copy and sanitize extension (up to 3 chars)
    if (dot) { // Only process if extension exists
        for (i = 0, j = 0; j < 3 && i < len_ext; ++i) {
            char c = toupper((unsigned char)dot[1 + i]);
            if (c == '.' || c == ' ') continue; // Skip dots/spaces within ext
            if (strchr("\"*+,/:;<=>?[\\]|", c) || c < 0x20) c = '_';
            ext[j++] = c;
        }
    }
    // Pad remaining ext with spaces if needed (already done by init)

    // 3. Try BASE    .EXT first
    uint8_t candidate[11];
    memcpy(candidate, base, 8);
    memcpy(candidate + 8, ext, 3);
    if (!fat_raw_short_name_exists(fs, parent_dir_cluster, candidate)) {
        memcpy(short_name_out, candidate, 11);
        FAT_DEBUG_LOG("Generated unique 8.3: '%.11s' (no suffix needed)", candidate);
        return FS_SUCCESS;
    }

    // 4. Try BASE~N.EXT variations (1 <= N <= 999999)
    char num_suffix[8]; // ~ + 6 digits + null
    for (int n = 1; n <= 999999; ++n) {
        num_suffix[0] = '~';
        int num_len = _itoa_simple(n, num_suffix + 1, sizeof(num_suffix) - 1);
        if (num_len < 0) {
            FAT_ERROR_LOG("Failed to convert suffix number %d to string.", n);
            return FS_ERR_INTERNAL; // Should not happen
        }
        int suffix_len = 1 + num_len; // Length including '~'

        // Determine how many base characters to keep
        int base_chars_to_keep = 8 - suffix_len;
        if (base_chars_to_keep < 1) base_chars_to_keep = 1; // Keep at least 1 char

        // Construct the trial name
        memcpy(candidate, base, base_chars_to_keep); // Copy truncated base
        memcpy(candidate + base_chars_to_keep, num_suffix, suffix_len); // Copy suffix
        // Pad remaining base name part with spaces if needed
        for (int k = base_chars_to_keep + suffix_len; k < 8; ++k) {
            candidate[k] = ' ';
        }
        memcpy(candidate + 8, ext, 3); // Copy original extension

        // Check uniqueness
        if (!fat_raw_short_name_exists(fs, parent_dir_cluster, candidate)) {
            memcpy(short_name_out, candidate, 11);
            FAT_DEBUG_LOG("Generated unique 8.3: '%.11s' (using suffix ~%d)", candidate, n);
            return FS_SUCCESS;
        }
    }

    FAT_ERROR_LOG("Could not generate unique short name for '%s' after %d attempts.", long_name, 999999);
    return FS_ERR_NO_SPACE; // Or FS_ERR_FILE_EXISTS? NO_SPACE seems more appropriate
}


/* --- Timestamp --- */

/**
 * @brief Gets the current time and date in FAT timestamp format.
 * @note THIS IS A PLACEHOLDER - Replace with actual RTC/Time source access.
 */
void fat_get_current_timestamp(uint16_t *fat_time, uint16_t *fat_date) {
    KERNEL_ASSERT(fat_time != NULL && fat_date != NULL, "NULL output pointer in fat_get_current_timestamp");

    // --- Placeholder Implementation ---
    // Replace this section with code that reads from your kernel's time source (RTC, etc.)
    // For now, using a fixed date/time: May 5, 2025, 14:40:00 (matches user context)
    uint16_t year = 2025;
    uint16_t month = 5;  // 1-based month
    uint16_t day = 5;
    uint16_t hours = 14; // 24-hour format
    uint16_t minutes = 40;
    uint16_t seconds = 0;
    // --- End Placeholder ---

    // Clamp year to FAT epoch start if necessary
    if (year < 1980) year = 1980;

    // Pack date: ((Year - 1980) << 9) | (Month << 5) | Day
    *fat_date = (uint16_t)(((year - 1980) & 0x7F) << 9) | // 7 bits for year offset
                (uint16_t)((month & 0x0F) << 5) |         // 4 bits for month
                (uint16_t)(day & 0x1F);                    // 5 bits for day

    // Pack time: (Hours << 11) | (Minutes << 5) | (Seconds / 2)
    *fat_time = (uint16_t)((hours & 0x1F) << 11) |         // 5 bits for hours
                (uint16_t)((minutes & 0x3F) << 5) |       // 6 bits for minutes
                (uint16_t)((seconds >> 1) & 0x1F);        // 5 bits for seconds/2

    FAT_DEBUG_LOG("Returning fixed timestamp: Date=0x%04x, Time=0x%04x", *fat_date, *fat_time);
}


/* --- Static Helper Implementation for itoa --- */
/**
 * @brief Simple integer to ASCII conversion helper (positive integers only).
 * Writes the string representation of 'value' into 'buf'.
 * @param value The positive integer to convert.
 * @param buf Output buffer.
 * @param max_len Maximum size of the output buffer (including null terminator).
 * @return Length of the generated string (excluding null), or -1 on error.
 */
static int _itoa_simple(int value, char* buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    if (value == 0) {
        if (max_len < 2) return -1;
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    if (value < 0) {
        FAT_ERROR_LOG("_itoa_simple does not handle negative numbers.");
        return -1; // Not handled
    }

    int i = 0;
    int temp_val = value;

    // Calculate digits needed (store in reverse order temporarily)
    while (temp_val > 0 && i < max_len - 1) {
        buf[i++] = (temp_val % 10) + '0';
        temp_val /= 10;
    }

    if (temp_val > 0) {
        FAT_ERROR_LOG("_itoa_simple buffer too small for value %d (max_len %d)", value, max_len);
        return -1; // Buffer wasn't long enough
    }

    buf[i] = '\0'; // Null-terminate
    int len = i;

    // Reverse the string in place
    int start = 0;
    int end = len - 1;
    while (start < end) {
        char temp_char = buf[start];
        buf[start] = buf[end];
        buf[end] = temp_char;
        start++;
        end--;
    }
    return len; // Return length of string
}