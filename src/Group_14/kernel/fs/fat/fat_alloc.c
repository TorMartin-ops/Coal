/**
 * @file fat_alloc.c
 * @brief Cluster allocation and management for the FAT filesystem driver.
 * @author Tor Martin Kohle
 * @version 1.5
 */

// --- Core Includes ---
#include <kernel/fs/fat/fat_alloc.h>
#include <kernel/fs/fat/fat_core.h>       // fat_fs_t definition
#include <kernel/fs/fat/fat_fs.h>         // General FAT structures (may overlap with fat_core.h, review for minimal set)
#include <kernel/fs/fat/fat_utils.h>      // For get/set_cluster_entry, fat_get_entry_cluster, fat_generate_short_name, fat_get_current_timestamp etc.
#include <kernel/fs/fat/fat_dir.h>        // For find_free_directory_slot, write_directory_entries, update_directory_entry, fat_lookup_path etc.
#include <kernel/fs/fat/fat_lfn.h>        // For fat_generate_lfn_entries, fat_calculate_lfn_checksum etc.
#include <kernel/fs/vfs/fs_util.h>        // For fs_util_split_path
#include <kernel/fs/vfs/fs_config.h>      // FS_MAX_PATH_LENGTH, MAX_FILENAME_LEN
#include <kernel/fs/vfs/fs_errno.h>       // Filesystem error codes
#include <kernel/drivers/display/terminal.h>       // terminal_printf for logging
#include <kernel/memory/kmalloc.h>        // kmalloc/kfree
#include <kernel/lib/assert.h>         // KERNEL_ASSERT
#include <kernel/lib/string.h>         // strlen, strcpy, memset, memcpy, strcmp

// --- Standard Type Includes ---
#include <libc/stdbool.h>
#include <libc/stdint.h>
// #include "libc/stddef.h" // Typically brought in by other headers like string.h if needed for size_t

// --- Logging Macros ---
#ifdef KLOG_LEVEL_DEBUG
#define FAT_ALLOC_DEBUG(fmt, ...) serial_printf("[fat_alloc:DBG] %s: " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_ALLOC_DEBUG(fmt, ...) ((void)0)
#endif
#define FAT_ALLOC_INFO(fmt, ...)  serial_printf("[fat_alloc:INFO] %s: " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ALLOC_WARN(fmt, ...)  serial_printf("[fat_alloc:WARN] %s: " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ALLOC_ERROR(fmt, ...) serial_printf("[fat_alloc:ERR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
// --- End Logging Macros ---


/**
 * @brief Finds the first available (zero-entry) cluster in the FAT table.
 * @param fs Pointer to the FAT filesystem structure.
 * @return The cluster number if found (>=2), or 0 on error or if no free cluster.
 * @note Assumes caller holds fs->lock.
 */
static uint32_t find_free_cluster(fat_fs_t *fs)
{
    KERNEL_ASSERT(fs != NULL, "NULL fs pointer");

    if (!fs->fat_table) {
        FAT_ALLOC_ERROR("FAT table not loaded.");
        return 0;
    }

    // Start search from cluster 2 up to the last data cluster.
    // fs->total_data_clusters is count of data clusters, so highest cluster num is total_data_clusters + 1.
    uint32_t last_search_cluster = fs->total_data_clusters + 1;

    for (uint32_t cluster_num = 2; cluster_num <= last_search_cluster; cluster_num++) {
        uint32_t entry_value;
        if (fat_get_cluster_entry(fs, cluster_num, &entry_value) != FS_SUCCESS) {
            FAT_ALLOC_ERROR("Failed to read FAT entry for cluster %lu", (unsigned long)cluster_num);
            return 0; // Indicate read failure
        }

        if (entry_value == 0) { // Cluster is free
            FAT_ALLOC_DEBUG("Found free cluster: %lu", (unsigned long)cluster_num);
            return cluster_num;
        }
    }

    FAT_ALLOC_WARN("No free clusters found on device.");
    return 0; // No free clusters
}


/**
 * @brief Allocates a new cluster, marks it as EOC, and optionally links it from a previous cluster.
 * @param fs Pointer to the FAT filesystem structure.
 * @param previous_cluster The cluster number to link from (0 if this is the first cluster).
 * @return The newly allocated cluster number (>=2), or 0 on failure.
 * @note Assumes caller holds fs->lock.
 */
uint32_t fat_allocate_cluster(fat_fs_t *fs, uint32_t previous_cluster)
{
    KERNEL_ASSERT(fs != NULL, "FAT filesystem context cannot be NULL");

    if (!fs->fat_table) {
        FAT_ALLOC_ERROR("FAT table not loaded.");
        return 0;
    }

    uint32_t new_cluster = find_free_cluster(fs);
    if (new_cluster < 2) { // find_free_cluster returns 0 on error or no space, cluster < 2 is invalid
        FAT_ALLOC_WARN("find_free_cluster failed or no space available.");
        return 0;
    }
    FAT_ALLOC_DEBUG("Found free cluster %lu to allocate.", (unsigned long)new_cluster);

    // Mark the new cluster as End-Of-Chain in the FAT
    if (fat_set_cluster_entry(fs, new_cluster, fs->eoc_marker) != FS_SUCCESS) {
        FAT_ALLOC_ERROR("Failed to mark cluster %lu as EOC.", (unsigned long)new_cluster);
        // Technically, the cluster is still marked free if this fails immediately, but it's an error state.
        return 0;
    }
    FAT_ALLOC_DEBUG("Marked cluster %lu as EOC.", (unsigned long)new_cluster);

    // If there's a previous cluster, link it to the new one
    if (previous_cluster >= 2) {
        FAT_ALLOC_DEBUG("Linking previous cluster %lu to new cluster %lu.", (unsigned long)previous_cluster, (unsigned long)new_cluster);
        if (fat_set_cluster_entry(fs, previous_cluster, new_cluster) != FS_SUCCESS) {
            FAT_ALLOC_ERROR("Failed to link cluster %lu -> %lu.", (unsigned long)previous_cluster, (unsigned long)new_cluster);
            // Rollback: Mark the new_cluster as free again since linking failed.
            fat_set_cluster_entry(fs, new_cluster, 0); // Best effort rollback
            return 0;
        }
        FAT_ALLOC_DEBUG("Successfully linked cluster %lu -> %lu.", (unsigned long)previous_cluster, (unsigned long)new_cluster);
    } else {
        FAT_ALLOC_DEBUG("No valid previous cluster (%lu), allocating as a new chain start.", (unsigned long)previous_cluster);
    }

    FAT_ALLOC_INFO("Successfully allocated cluster %lu.", (unsigned long)new_cluster);
    return new_cluster;
}


/**
 * @brief Frees an entire cluster chain in the FAT, starting from a given cluster.
 * @param fs Pointer to the FAT filesystem structure.
 * @param start_cluster The first cluster in the chain to free. Must be >= 2.
 * @return FS_SUCCESS if successful, or an error code.
 * @note Assumes caller holds fs->lock.
 */
int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster)
{
    KERNEL_ASSERT(fs != NULL, "FAT filesystem context cannot be NULL");
    FAT_ALLOC_DEBUG("Freeing chain starting from cluster %lu", (unsigned long)start_cluster);

    if (start_cluster < 2) {
        FAT_ALLOC_ERROR("Cannot free reserved/invalid cluster %lu", (unsigned long)start_cluster);
        return FS_ERR_INVALID_PARAM;
    }
    if (!fs->fat_table) {
        FAT_ALLOC_ERROR("FAT table not loaded.");
        return FS_ERR_IO;
    }

    uint32_t current_cluster = start_cluster;
    int result = FS_SUCCESS;

    while (current_cluster >= 2 && current_cluster < fs->eoc_marker) {
        uint32_t next_cluster_val = 0; // Initialize to an invalid/non-chain value
        FAT_ALLOC_DEBUG("Processing cluster %lu in chain.", (unsigned long)current_cluster);

        // Get the next cluster in the chain before freeing the current one
        if (fat_get_next_cluster(fs, current_cluster, &next_cluster_val) != FS_SUCCESS) {
            FAT_ALLOC_WARN("Error reading FAT entry for cluster %lu. Stopping chain free.", (unsigned long)current_cluster);
            result = FS_ERR_IO; // Or the error from fat_get_next_cluster
            break;
        }
        FAT_ALLOC_DEBUG("Next cluster in chain is %lu.", (unsigned long)next_cluster_val);

        // Mark current cluster as free (0)
        if (fat_set_cluster_entry(fs, current_cluster, 0) != FS_SUCCESS) {
            FAT_ALLOC_WARN("Error writing FAT entry to free cluster %lu.", (unsigned long)current_cluster);
            if (result == FS_SUCCESS) result = FS_ERR_IO; // Preserve earlier error if any
            break;
        }
        FAT_ALLOC_DEBUG("Marked cluster %lu as free.", (unsigned long)current_cluster);

        current_cluster = next_cluster_val;

        // Safety check for invalid chain links (should not point to 0 or 1 mid-chain)
        if (current_cluster == 0 || current_cluster == 1) {
            FAT_ALLOC_ERROR("Corrupt FAT chain detected (link to %lu from previous).", (unsigned long)current_cluster);
            if (result == FS_SUCCESS) result = FS_ERR_CORRUPT;
            break;
        }
    }

    if (result == FS_SUCCESS) {
        FAT_ALLOC_DEBUG("Chain free process completed successfully for start_cluster %lu.", (unsigned long)start_cluster);
    } else {
        FAT_ALLOC_WARN("Chain free process for start_cluster %lu finished with error %d.", (unsigned long)start_cluster, result);
    }
    return result;
}

/**
 * @brief Creates a new file or directory entry (including LFNs) in the specified parent directory.
 * @param fs Filesystem instance.
 * @param path Full path to the new entry.
 * @param attributes Attributes for the new entry (e.g., FAT_ATTR_DIRECTORY).
 * @param entry_out (Output) The created 8.3 directory entry.
 * @param dir_cluster_out (Output) The cluster number of the directory containing the new entry.
 * @param dir_offset_out (Output) The byte offset of the 8.3 entry within its directory sector.
 * @return FS_SUCCESS on success, or an error code.
 * @note Assumes caller holds fs->lock.
 */
int fat_create_file(fat_fs_t        *fs,
                      const char      *path,
                      uint8_t          attributes,
                      fat_dir_entry_t *entry_out,         /* out */
                      uint32_t        *dir_cluster_out,   /* out */
                      uint32_t        *dir_offset_out)    /* out */
{
    FAT_ALLOC_DEBUG("Path='%s', Attr=0x%02x", path ? path : "<NULL>", attributes);
    KERNEL_ASSERT(fs && path && entry_out && dir_cluster_out && dir_offset_out, "Invalid arguments");

    int ret;
    char parent_path[FS_MAX_PATH_LENGTH];
    char filename[MAX_FILENAME_LEN + 1];

    // 1. Split path into parent directory and filename
    if (fs_util_split_path(path, parent_path, sizeof(parent_path), filename, sizeof(filename)) != 0) {
        FAT_ALLOC_ERROR("Path '%s' is too long or invalid.", path);
        return FS_ERR_NAMETOOLONG;
    }
    if (filename[0] == '\0') {
        FAT_ALLOC_ERROR("Cannot create with empty filename in path '%s'.", path);
        return FS_ERR_INVALID_PARAM;
    }
    FAT_ALLOC_DEBUG("Parent='%s', Filename='%s'", parent_path, filename);

    // 2. Resolve parent directory cluster
    fat_dir_entry_t parent_dir_sfn_entry; // SFN entry of parent dir
    uint32_t parent_dir_actual_cluster;
    if (strcmp(parent_path, ".") == 0 || strcmp(parent_path, "/") == 0 || parent_path[0] == '\0') { // Root directory
        parent_dir_actual_cluster = (fs->type == FAT_TYPE_FAT32) ? fs->root_cluster : 0;
    } else {
        uint32_t ignored_cluster, ignored_offset; // Not needed for parent itself here
        ret = fat_lookup_path(fs, parent_path, &parent_dir_sfn_entry, NULL, 0, &ignored_cluster, &ignored_offset);
        if (ret != FS_SUCCESS) {
            FAT_ALLOC_ERROR("Parent dir '%s' lookup failed (err %d).", parent_path, ret);
            return ret;
        }
        if (!(parent_dir_sfn_entry.attr & FAT_ATTR_DIRECTORY)) {
            FAT_ALLOC_ERROR("Parent path '%s' is not a directory.", parent_path);
            return FS_ERR_NOT_A_DIRECTORY;
        }
        parent_dir_actual_cluster = fat_get_entry_cluster(&parent_dir_sfn_entry);
    }
    FAT_ALLOC_DEBUG("Parent dir cluster: %lu", (unsigned long)parent_dir_actual_cluster);

    // 3. Generate short name and LFN entries
    uint8_t short_name_raw[11];
    ret = fat_generate_short_name(fs, parent_dir_actual_cluster, filename, short_name_raw);
    if (ret != FS_SUCCESS) {
        FAT_ALLOC_ERROR("Failed to generate unique short name for '%s' (err %d).", filename, ret);
        return ret;
    }
    // FAT_ALLOC_DEBUG("Generated short name: '%.11s'", short_name_raw); // Raw, not null-terminated

    fat_lfn_entry_t lfn_entries_buf[FAT_MAX_LFN_ENTRIES];
    uint8_t lfn_checksum = fat_calculate_lfn_checksum(short_name_raw);
    int num_lfn_slots = fat_generate_lfn_entries(filename, lfn_checksum, lfn_entries_buf, FAT_MAX_LFN_ENTRIES);
    if (num_lfn_slots < 0) {
        FAT_ALLOC_ERROR("Failed to generate LFN entries for '%s' (err %d).", filename, num_lfn_slots);
        return num_lfn_slots;
    }
    size_t total_slots_to_write = (size_t)num_lfn_slots + 1; // +1 for the SFN entry
    FAT_ALLOC_DEBUG("%d LFN entries (total %zu slots). Checksum: 0x%02x", num_lfn_slots, total_slots_to_write, lfn_checksum);

    // 4. Find sufficient free space in parent directory for all entries
    uint32_t free_slot_dir_cluster, free_slot_offset_start;
    ret = find_free_directory_slot(fs, parent_dir_actual_cluster, total_slots_to_write, &free_slot_dir_cluster, &free_slot_offset_start);
    if (ret != FS_SUCCESS) {
        FAT_ALLOC_ERROR("Failed to find %zu free slots in dir cluster %lu (err %d).", total_slots_to_write, (unsigned long)parent_dir_actual_cluster, ret);
        return ret;
    }
    FAT_ALLOC_DEBUG("Found %zu free slots at DirCluster=%lu, StartOffset=%lu", total_slots_to_write, (unsigned long)free_slot_dir_cluster, (unsigned long)free_slot_offset_start);

    // 5. Prepare the 8.3 (SFN) directory entry
    fat_dir_entry_t sfn_entry_to_write;
    memset(&sfn_entry_to_write, 0, sizeof(sfn_entry_to_write));
    memcpy(sfn_entry_to_write.name, short_name_raw, 11);
    sfn_entry_to_write.attr = attributes | FAT_ATTR_ARCHIVE; // New files/dirs should have archive bit
    sfn_entry_to_write.file_size = 0;
    sfn_entry_to_write.first_cluster_low = 0;
    sfn_entry_to_write.first_cluster_high = 0;

    // TODO: Ensure fat_get_current_timestamp is implemented and declared correctly.
    // This is a placeholder call as per original code's note.
    uint16_t current_fat_time, current_fat_date;
    fat_get_current_timestamp(&current_fat_time, &current_fat_date);
    sfn_entry_to_write.creation_time = current_fat_time;
    sfn_entry_to_write.creation_date = current_fat_date;
    sfn_entry_to_write.last_access_date = current_fat_date;
    sfn_entry_to_write.write_time = current_fat_time;
    sfn_entry_to_write.write_date = current_fat_date;

    // 6. Write LFN entries (if any) then the SFN entry
    uint32_t current_write_offset = free_slot_offset_start;
    if (num_lfn_slots > 0) {
        FAT_ALLOC_DEBUG("Writing %d LFN entries...", num_lfn_slots);
        ret = write_directory_entries(fs, free_slot_dir_cluster, current_write_offset, lfn_entries_buf, (size_t)num_lfn_slots);
        if (ret != FS_SUCCESS) {
            FAT_ALLOC_ERROR("Failed to write LFN entries (err %d).", ret);
            // TODO: Consider rollback of any directory extensions made by find_free_directory_slot.
            return ret;
        }
        current_write_offset += num_lfn_slots * sizeof(fat_dir_entry_t);
    }
    FAT_ALLOC_DEBUG("Writing SFN entry at offset %lu...", (unsigned long)current_write_offset);
    ret = write_directory_entries(fs, free_slot_dir_cluster, current_write_offset, &sfn_entry_to_write, 1);
    if (ret != FS_SUCCESS) {
        FAT_ALLOC_ERROR("Failed to write SFN entry (err %d).", ret);
        // TODO: Consider rollback.
        return ret;
    }

    // 7. Success: Output details of the created SFN entry
    *dir_cluster_out = free_slot_dir_cluster;      // Directory where entry was placed
    *dir_offset_out = current_write_offset;      // Offset of the SFN part of the entry
    if (entry_out) memcpy(entry_out, &sfn_entry_to_write, sizeof(sfn_entry_to_write));

    FAT_ALLOC_INFO("Created entry for '%s' at DirCluster=%lu, SFN_Offset=%lu",
                     filename, (unsigned long)free_slot_dir_cluster, (unsigned long)current_write_offset);
    return FS_SUCCESS;
}

/**
 * @brief Truncates a file to zero length by freeing its cluster chain and updating its directory entry.
 * @param fs Filesystem instance.
 * @param entry (In/Out) Pointer to the file's directory entry structure. This will be modified.
 * @param entry_dir_cluster Cluster number of the directory containing this entry.
 * @param entry_offset_in_dir Byte offset of this entry within its directory.
 * @return FS_SUCCESS on success, or an error code.
 * @note Assumes caller holds fs->lock.
 */
int fat_truncate_file(fat_fs_t *fs,
                      fat_dir_entry_t *entry, // In/Out
                      uint32_t entry_dir_cluster,
                      uint32_t entry_offset_in_dir)
{
    // Note: entry->name is not null-terminated, handle with care in logs if printing as string.
    FAT_ALLOC_DEBUG("Truncating file (SFN starts '%.*s'), DirCluster=%lu, DirOffset=%lu",
                     (int)sizeof(entry->name), entry ? (char*)entry->name : "NULL_ENTRY",
                     (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
    KERNEL_ASSERT(fs != NULL && entry != NULL, "NULL pointer argument");

    if (entry->attr & FAT_ATTR_DIRECTORY) {
        FAT_ALLOC_ERROR("Attempted to truncate a directory.");
        return FS_ERR_IS_A_DIRECTORY;
    }

    // 1. Free the file's cluster chain, if any
    uint32_t first_data_cluster = fat_get_entry_cluster(entry);
    uint32_t original_size = entry->file_size;
    FAT_ALLOC_DEBUG("File's first cluster = %lu, size = %lu", (unsigned long)first_data_cluster, (unsigned long)original_size);

    if (first_data_cluster >= 2 && original_size > 0) {
        FAT_ALLOC_INFO("Freeing cluster chain from %lu.", (unsigned long)first_data_cluster);
        int free_ret = fat_free_cluster_chain(fs, first_data_cluster);
        if (free_ret != FS_SUCCESS) {
            FAT_ALLOC_ERROR("fat_free_cluster_chain failed for cluster %lu (err %d).", (unsigned long)first_data_cluster, free_ret);
            return free_ret; // Propagate error, directory entry not updated.
        }
        FAT_ALLOC_DEBUG("Cluster chain from %lu freed.", (unsigned long)first_data_cluster);
    } else {
        FAT_ALLOC_DEBUG("No data clusters to free (first_cluster=%lu, size=%lu).", (unsigned long)first_data_cluster, (unsigned long)original_size);
    }

    // 2. Update the in-memory directory entry fields
    entry->file_size = 0;
    entry->first_cluster_low = 0;
    entry->first_cluster_high = 0;

    // TODO: CRITICAL - fat_get_current_timestamp must be implemented and declared.
    // The original code had a note about this. This is a placeholder.
    // Without a working timestamp function, these fields will not be correctly updated.
    uint16_t current_fat_time, current_fat_date;
    fat_get_current_timestamp(&current_fat_time, &current_fat_date); // Placeholder
    entry->write_time = current_fat_time;
    entry->write_date = current_fat_date;
    entry->last_access_date = current_fat_date; // Typically updated on access, but write implies access.
    FAT_ALLOC_DEBUG("In-memory entry updated: size=0, cluster=0, time=0x%04x, date=0x%04x", entry->write_time, entry->write_date);

    // 3. Write the modified directory entry back to disk
    FAT_ALLOC_DEBUG("Updating SFN entry on disk at DirCluster %lu, Offset %lu", (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
    int update_ret = update_directory_entry(fs, entry_dir_cluster, entry_offset_in_dir, entry);
    if (update_ret != FS_SUCCESS) {
        FAT_ALLOC_ERROR("update_directory_entry failed (err %d). File state may be inconsistent.", update_ret);
        // At this point, clusters might be freed but dir entry not updated.
        return update_ret;
    }

    FAT_ALLOC_INFO("File successfully truncated (SFN starts '%.*s').", (int)sizeof(entry->name), (char*)entry->name);
    return FS_SUCCESS;
}

// --- End of file fat_alloc.c ---