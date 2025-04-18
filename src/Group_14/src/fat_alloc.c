/**
 * @file fat_alloc.c
 * @brief Cluster allocation and management implementation for FAT filesystem
 */

// --- Core Includes (Ensure these are correct and sufficient) ---
#include "fat_alloc.h"     // Our declarations
#include "fat_core.h"      // fat_fs_t definition
#include "fat_fs.h"        // fat_dir_entry_t, fat_lfn_entry_t, fat_file_context_t etc.
#include "fat_utils.h"     // Needs declarations for get/set_cluster_entry, get_next_cluster, fat_generate_short_name, fat_get_current_timestamp etc.
#include "fat_dir.h"       // Needs declarations for find_free_directory_slot, write_directory_entries, update_directory_entry, fat_lookup_path, FAT_MAX_LFN_ENTRIES etc.
#include "fat_lfn.h"       // Needs declarations for fat_generate_lfn_entries, fat_calculate_lfn_checksum etc.
#include "fs_util.h"       // Needs declaration for fs_util_split_path
#include "fs_config.h"     // Needs definition for FS_MAX_PATH_LENGTH, MAX_FILENAME_LEN
#include "fs_errno.h"      // Error codes
#include "terminal.h"      // terminal_printf / logging
#include "kmalloc.h"       // kmalloc/kfree
#include "assert.h"        // KERNEL_ASSERT
#include "string.h"        // strlen, strcpy, memset, memcpy, strcmp (Use project's string.h)

// --- Standard Type Includes (ensure these paths are correct for your libc includes) ---
#include "libc/stdbool.h"
#include "libc/stdint.h"
// #include "libc/stddef.h" // Include if needed for size_t


// --- Logging Macros (Define properly elsewhere or keep here temporarily) ---
#ifdef KLOG_LEVEL_DEBUG
#define FAT_ALLOC_DEBUG_LOG(fmt, ...) terminal_printf("[fat_alloc:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_ALLOC_DEBUG_LOG(fmt, ...) do {} while(0)
#endif
#define FAT_ALLOC_INFO_LOG(fmt, ...)  terminal_printf("[fat_alloc:INFO]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ALLOC_WARN_LOG(fmt, ...)  terminal_printf("[fat_alloc:WARN]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ALLOC_ERROR_LOG(fmt, ...) terminal_printf("[fat_alloc:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
// --- End Logging Macros ---


/**
 * @brief Finds the first available free cluster in the FAT table.
 */
static uint32_t find_free_cluster(fat_fs_t *fs)
{
    KERNEL_ASSERT(fs != NULL, "NULL fs pointer in find_free_cluster");
    // Assumes caller holds fs->lock

    if (!fs->fat_table) {
        FAT_ALLOC_ERROR_LOG("FAT table not loaded.");
        return 0;
    }

    uint32_t first_search_cluster = 2;
    uint32_t last_search_cluster = fs->total_data_clusters + 1; // total_data_clusters is typically highest cluster number + 1

    for (uint32_t i = first_search_cluster; i <= last_search_cluster; i++) {
        uint32_t entry_value;
        // NOTE: Requires fat_get_cluster_entry to be correctly declared and implemented
        if (fat_get_cluster_entry(fs, i, &entry_value) != FS_SUCCESS) {
            // Corrected format specifier %u -> %lu
            FAT_ALLOC_ERROR_LOG("Failed to read FAT entry for cluster %lu", (unsigned long)i);
            return 0; // Indicate failure
        }

        if (entry_value == 0) {
             // Corrected format specifier %u -> %lu
            FAT_ALLOC_DEBUG_LOG("Found free cluster: %lu", (unsigned long)i);
            return i; // Found a free cluster
        }
    }

    FAT_ALLOC_WARN_LOG("No free clusters found on device.");
    return 0; // No free clusters found
}


/**
 * @brief Allocates a new cluster and optionally links it to a previous one.
 */
uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster)
{
    KERNEL_ASSERT(fs != NULL, "FAT filesystem context cannot be NULL in fat_allocate_cluster");
    // Assumes caller holds fs->lock

    if (!fs->fat_table) {
        FAT_ALLOC_ERROR_LOG("FAT table not loaded.");
        return 0;
    }

    uint32_t free_cluster = find_free_cluster(fs);
    if (free_cluster < 2) {
        FAT_ALLOC_WARN_LOG("find_free_cluster failed or returned no space.");
        return 0;
    }
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Found free cluster %lu to allocate.", (unsigned long)free_cluster);

    // NOTE: Requires fat_set_cluster_entry to be correctly declared and implemented
    int set_eoc_res = fat_set_cluster_entry(fs, free_cluster, fs->eoc_marker);
    if (set_eoc_res != FS_SUCCESS) {
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_ERROR_LOG("Failed to mark cluster %lu as EOC (err %d)", (unsigned long)free_cluster, set_eoc_res);
        return 0;
    }
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Marked cluster %lu as EOC.", (unsigned long)free_cluster);

    if (previous_cluster >= 2) {
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Linking previous cluster %lu to new cluster %lu.", (unsigned long)previous_cluster, (unsigned long)free_cluster);
        // NOTE: Requires fat_set_cluster_entry
        int link_res = fat_set_cluster_entry(fs, previous_cluster, free_cluster);
        if (link_res != FS_SUCCESS) {
            // Corrected format specifier %u -> %lu
            FAT_ALLOC_ERROR_LOG("Failed to link cluster %lu -> %lu (err %d)",
                                (unsigned long)previous_cluster, (unsigned long)free_cluster, link_res);
            // Corrected format specifier %u -> %lu
            FAT_ALLOC_WARN_LOG("Attempting rollback: Marking cluster %lu back to free.", (unsigned long)free_cluster);
            fat_set_cluster_entry(fs, free_cluster, 0); // Best effort rollback
            return 0;
        }
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Successfully linked cluster %lu -> %lu.", (unsigned long)previous_cluster, (unsigned long)free_cluster);
    } else {
         // Corrected format specifier %u -> %lu
         FAT_ALLOC_DEBUG_LOG("No valid previous cluster (%lu), allocating first cluster.", (unsigned long)previous_cluster);
    }

    // Corrected format specifier %u -> %lu
    FAT_ALLOC_INFO_LOG("Successfully allocated cluster %lu.", (unsigned long)free_cluster);
    return free_cluster;
}


/**
 * @brief Frees an entire cluster chain starting from a given cluster.
 */
int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster)
{
    KERNEL_ASSERT(fs != NULL, "FAT filesystem context cannot be NULL in fat_free_cluster_chain");
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Enter: Freeing chain starting from cluster %lu", (unsigned long)start_cluster);

    if (start_cluster < 2) {
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_ERROR_LOG("Cannot free reserved cluster %lu", (unsigned long)start_cluster);
        return -FS_ERR_INVALID_PARAM;
    }
     if (!fs->fat_table) {
        FAT_ALLOC_ERROR_LOG("FAT table not loaded.");
        return -FS_ERR_IO;
    }

    // Assumes caller holds fs->lock

    uint32_t current_cluster = start_cluster;
    int result = FS_SUCCESS;

    while (current_cluster >= 2 && current_cluster < fs->eoc_marker) {
        uint32_t next_cluster = 0;
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Processing cluster %lu in chain.", (unsigned long)current_cluster);

        // NOTE: Requires fat_get_next_cluster to be correctly declared and implemented
        int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_cluster);
        if (get_next_res != FS_SUCCESS) {
             // Corrected format specifier %u -> %lu
             FAT_ALLOC_WARN_LOG("Error reading FAT entry for cluster %lu (err %d). Stopping chain free.",
                                (unsigned long)current_cluster, get_next_res);
             result = get_next_res;
             break;
        }
        // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Next cluster in chain is %lu.", (unsigned long)next_cluster);

        // NOTE: Requires fat_set_cluster_entry
        int set_free_res = fat_set_cluster_entry(fs, current_cluster, 0);
        if (set_free_res != FS_SUCCESS) {
             // Corrected format specifier %u -> %lu
             FAT_ALLOC_WARN_LOG("Error writing FAT entry for cluster %lu (err %d).",
                                (unsigned long)current_cluster, set_free_res);
             if (result == FS_SUCCESS) result = set_free_res;
             break;
        }
         // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Marked cluster %lu as free.", (unsigned long)current_cluster);

        current_cluster = next_cluster;

         if (current_cluster == 0 || current_cluster == 1) { // Safety check
             // Corrected format specifier %u -> %lu
             FAT_ALLOC_ERROR_LOG("Corrupt FAT chain detected (link to %lu).", (unsigned long)current_cluster);
             if (result == FS_SUCCESS) result = -FS_ERR_CORRUPT;
             break;
         }
    }
     if (current_cluster != 0 && !(current_cluster >= 2 && current_cluster < fs->eoc_marker)) {
         // Corrected format specifier %u -> %lu
         FAT_ALLOC_DEBUG_LOG("Chain traversal finished. Last read next_cluster was %lu.", (unsigned long)current_cluster);
     }

    FAT_ALLOC_DEBUG_LOG("Exit: Chain free process finished with status %d", result);
    return result;
}


/**
 * @brief Creates a new file entry (including LFN and 8.3) in a parent directory.
 */
int fat_create_file(fat_fs_t *fs,
                    const char *path,
                    uint8_t attributes,
                    fat_dir_entry_t *entry_out,
                    uint32_t *dir_cluster_out, // Cluster containing the *parent* directory
                    uint32_t *dir_offset_out)  // Offset of the new 8.3 entry within parent
{
    FAT_ALLOC_DEBUG_LOG("Enter: path='%s', attr=0x%02x", path ? path : "NULL", attributes);
    KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && dir_cluster_out != NULL && dir_offset_out != NULL,
                  "NULL pointer argument in fat_create_file");
    // Assumes caller holds fs->lock

    int ret = FS_SUCCESS;
    char parent_path[FS_MAX_PATH_LENGTH]; // Needs fs_config.h
    char filename[MAX_FILENAME_LEN + 1];  // Needs fs_config.h
    fat_dir_entry_t parent_entry;
    uint32_t parent_cluster;
    uint32_t parent_entry_dir_cluster; // Unused
    uint32_t parent_entry_offset;      // Unused
    uint8_t short_name_raw[11];
    fat_lfn_entry_t lfn_entries[FAT_MAX_LFN_ENTRIES]; // Needs FAT_MAX_LFN_ENTRIES (fat_dir.h or fat_lfn.h)
    int lfn_count = 0;
    size_t needed_slots = 0;
    uint32_t slot_cluster = 0;
    uint32_t slot_offset = 0;

    // 1. Split path (Needs fs_util.h, string.h)
    if (fs_util_split_path(path, parent_path, sizeof(parent_path), filename, sizeof(filename)) != 0) {
        FAT_ALLOC_ERROR_LOG("Path '%s' or filename component too long.", path);
        return -FS_ERR_NAMETOOLONG;
    }
    if (strlen(filename) == 0) {
         FAT_ALLOC_ERROR_LOG("Cannot create file with empty filename from path '%s'.", path);
         return -FS_ERR_INVALID_PARAM;
    }
    FAT_ALLOC_DEBUG_LOG("Split path: parent='%s', filename='%s'", parent_path, filename);

    // 2. Lookup parent directory (Needs fat_dir.h, fat_utils.h, string.h)
    FAT_ALLOC_DEBUG_LOG("Looking up parent directory '%s'", parent_path);
    ret = fat_lookup_path(fs, parent_path, &parent_entry, NULL, 0,
                           &parent_entry_dir_cluster, &parent_entry_offset);
    if (ret != FS_SUCCESS) {
        FAT_ALLOC_ERROR_LOG("Parent directory lookup failed for '%s', error %d", parent_path, ret);
        return ret;
    }
    if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
        FAT_ALLOC_ERROR_LOG("Parent path '%s' is not a directory.", parent_path);
        return -FS_ERR_NOT_A_DIRECTORY;
    }
    parent_cluster = fat_get_entry_cluster(&parent_entry);
    if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_path, "/") == 0) {
        parent_cluster = 0;
    }
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Parent directory found, cluster = %lu", (unsigned long)parent_cluster);

    // 3. Generate names and LFN entries (Needs fat_utils.h, fat_lfn.h)
     // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Generating 8.3 name for '%s' in parent cluster %lu", filename, (unsigned long)parent_cluster);
    // >>>>>>>>>> NOTE: fat_generate_short_name is UNDEFINED <<<<<<<<<<<<<
    // You MUST implement this function (likely in fat_utils.c) and declare it (fat_utils.h)
    ret = fat_generate_short_name(fs, parent_cluster, filename, short_name_raw);
    if (ret != FS_SUCCESS) {
         FAT_ALLOC_ERROR_LOG("fat_generate_short_name failed for '%s', error %d", filename, ret);
         return ret;
    }
    uint8_t checksum = fat_calculate_lfn_checksum(short_name_raw);
    FAT_ALLOC_DEBUG_LOG("Generated 8.3 name '%.11s', checksum 0x%02x", short_name_raw, checksum);

    FAT_ALLOC_DEBUG_LOG("Generating LFN entries for '%s'", filename);
    lfn_count = fat_generate_lfn_entries(filename, checksum, lfn_entries, FAT_MAX_LFN_ENTRIES);
    if (lfn_count < 0) {
         FAT_ALLOC_ERROR_LOG("Failed to generate LFN entries for '%s', error %d", filename, lfn_count);
         return lfn_count;
    }
    needed_slots = (size_t)lfn_count + 1;
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Generated %d LFN entries, total slots needed: %lu", lfn_count, (unsigned long)needed_slots);

    // 4. Find free slots (Needs fat_dir.h)
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Finding %lu free slots in directory cluster %lu", (unsigned long)needed_slots, (unsigned long)parent_cluster);
    ret = find_free_directory_slot(fs, parent_cluster, needed_slots, &slot_cluster, &slot_offset);
    if (ret != FS_SUCCESS) {
        // Corrected format specifier %u -> %lu
         FAT_ALLOC_ERROR_LOG("Failed to find %lu free slots in directory cluster %lu, error %d", (unsigned long)needed_slots, (unsigned long)parent_cluster, ret);
         return ret;
    }
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Found %lu free slots starting at cluster %lu, offset %lu", (unsigned long)needed_slots, (unsigned long)slot_cluster, (unsigned long)slot_offset);

    // 5. Create the 8.3 entry struct (Needs string.h, fat_utils.h)
    fat_dir_entry_t entry_83;
    memset(&entry_83, 0, sizeof(entry_83));
    memcpy(entry_83.name, short_name_raw, 11);
    entry_83.attr = attributes | FAT_ATTR_ARCHIVE;
    entry_83.file_size = 0;
    // Corrected: Directly set cluster fields to 0
    entry_83.first_cluster_low = 0;
    entry_83.first_cluster_high = 0;
    uint16_t fat_time, fat_date;
    // >>>>>>>>>> NOTE: fat_get_current_timestamp is UNDEFINED <<<<<<<<<<<<<
    // You MUST implement this function (likely in fat_utils.c) and declare it (fat_utils.h)
    fat_get_current_timestamp(&fat_time, &fat_date);
    entry_83.creation_time = fat_time;
    entry_83.creation_date = fat_date;
    entry_83.last_access_date = fat_date;
    entry_83.write_time = fat_time; // Corrected name
    entry_83.write_date = fat_date; // Corrected name

    // 6. Write entries (Needs fat_dir.h)
    uint32_t current_write_offset = slot_offset;
    if (lfn_count > 0) {
         // Corrected format specifier %u -> %lu
         FAT_ALLOC_DEBUG_LOG("Writing %d LFN entries to cluster %lu, offset %lu", lfn_count, (unsigned long)slot_cluster, (unsigned long)current_write_offset);
         ret = write_directory_entries(fs, slot_cluster, current_write_offset, lfn_entries, (size_t)lfn_count);
         if (ret != FS_SUCCESS) {
             FAT_ALLOC_ERROR_LOG("Failed to write LFN entries, error %d", ret);
             return ret;
         }
         current_write_offset += (uint32_t)lfn_count * sizeof(fat_dir_entry_t);
    }

    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Writing 8.3 entry to cluster %lu, offset %lu", (unsigned long)slot_cluster, (unsigned long)current_write_offset);
    ret = write_directory_entries(fs, slot_cluster, current_write_offset, &entry_83, 1);
    if (ret != FS_SUCCESS) {
         FAT_ALLOC_ERROR_LOG("Failed to write 8.3 entry, error %d", ret);
         return ret;
    }

    // 7. Populate output parameters (Needs string.h)
    if (entry_out) {
         memcpy(entry_out, &entry_83, sizeof(fat_dir_entry_t));
    }
    *dir_cluster_out = slot_cluster;
    *dir_offset_out = current_write_offset;

    FAT_ALLOC_INFO_LOG("Successfully created file entry for '%s'", path);
    return FS_SUCCESS;
}


/**
 * @brief Truncates a file to zero length.
 */
int fat_truncate_file(fat_fs_t *fs,
                      fat_dir_entry_t *entry, // In/Out
                      uint32_t entry_dir_cluster,
                      uint32_t entry_offset_in_dir)
{
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Enter: entry=%p (name='%.11s'), dir_cluster=%lu, dir_offset=%lu",
                       entry, entry ? entry->name : (const uint8_t*)"NULL",
                       (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
    KERNEL_ASSERT(fs != NULL && entry != NULL, "NULL pointer argument in fat_truncate_file");
    // Assumes caller holds fs->lock

    int ret = FS_SUCCESS;

    if (entry->attr & FAT_ATTR_DIRECTORY) {
        FAT_ALLOC_ERROR_LOG("Attempted to truncate a directory '%.11s'", entry->name);
        return -FS_ERR_IS_A_DIRECTORY;
    }

    // 1. Free cluster chain (Needs fat_utils.h)
    uint32_t start_cluster = fat_get_entry_cluster(entry);
    uint32_t current_size = entry->file_size;
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("File '%.11s': start_cluster = %lu, size = %lu", entry->name, (unsigned long)start_cluster, (unsigned long)current_size);

    if (start_cluster >= 2 && current_size > 0) {
         // Corrected format specifier %u -> %lu
        FAT_ALLOC_INFO_LOG("Freeing cluster chain starting from %lu for file '%.11s'", (unsigned long)start_cluster, entry->name);
        ret = fat_free_cluster_chain(fs, start_cluster); // Assumes defined above
        if (ret != FS_SUCCESS) {
            FAT_ALLOC_ERROR_LOG("fat_free_cluster_chain failed for cluster %lu, error %d (%s)", (unsigned long)start_cluster, ret, fs_strerror(ret));
            return ret;
        }
         // Corrected format specifier %u -> %lu
        FAT_ALLOC_DEBUG_LOG("Cluster chain starting at %lu freed successfully.", (unsigned long)start_cluster);
    } else {
         // Corrected format specifier %u -> %lu
         FAT_ALLOC_DEBUG_LOG("No clusters needed freeing (start_cluster=%lu, size=%lu)", (unsigned long)start_cluster, (unsigned long)current_size);
    }

    // 2. Update entry in memory (Needs fat_utils.h)
    entry->file_size = 0;
    // Corrected: Directly set cluster fields to 0
    entry->first_cluster_low = 0;
    entry->first_cluster_high = 0;
    uint16_t fat_time, fat_date;
    // >>>>>>>>>> NOTE: fat_get_current_timestamp is UNDEFINED <<<<<<<<<<<<<
    // You MUST implement this function (likely in fat_utils.c) and declare it (fat_utils.h)
    fat_get_current_timestamp(&fat_time, &fat_date);
    // Use CORRECT struct member names
    entry->write_time = fat_time; // Corrected
    entry->write_date = fat_date; // Corrected
    entry->last_access_date = fat_date;
    FAT_ALLOC_DEBUG_LOG("Updated entry in memory: name='%.11s', size=0, cluster=0, time=0x%04x, date=0x%04x", entry->name, entry->write_time, entry->write_date);

    // 3. Write updated entry to disk (Needs fat_dir.h)
    // Corrected format specifier %u -> %lu
    FAT_ALLOC_DEBUG_LOG("Updating directory entry on disk at dir_cluster %lu, offset %lu", (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
    ret = update_directory_entry(fs, entry_dir_cluster, entry_offset_in_dir, entry);
    if (ret != FS_SUCCESS) {
         FAT_ALLOC_ERROR_LOG("update_directory_entry failed for '%.11s', error %d (%s)", entry->name, ret, fs_strerror(ret));
         return ret;
    }

    FAT_ALLOC_INFO_LOG("Successfully truncated file '%.11s'", entry->name);
    return FS_SUCCESS;
}

// --- End of file fat_alloc.c ---