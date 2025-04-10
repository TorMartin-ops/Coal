#include "fat.h"
#include "terminal.h"
#include "kmalloc.h"
#include "buddy.h"
#include "disk.h"
#include "vfs.h"
#include "fs_errno.h"
#include "fat_utils.h"      // Needs fat_cluster_to_lba, fat_get_next_cluster, fat_set_cluster_entry, format_filename
#include "buffer_cache.h"
#include "sys_file.h"       // Includes O_* flags (ensure definitions are consistent with vfs.h)
#include "types.h"
#include <string.h>         // For memcpy and memcmp

// --- Forward Declarations & Externs ---
extern int vfs_register_driver(vfs_driver_t *driver);
extern int vfs_unregister_driver(vfs_driver_t *driver);

// Filesystem operations
static void *fat_mount_internal(const char *device);
static int fat_unmount_internal(void *fs_context);
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
static int fat_read_internal(file_t *file, void *buf, size_t len);
static int fat_write_internal(file_t *file, const void *buf, size_t len);
static int fat_close_internal(file_t *file);

// Internal helpers
static int read_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, void *buf, size_t len);
static int write_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, const void *buf, size_t len);
static int find_directory_entry(fat_fs_t *fs, const char *path, fat_dir_entry_t *entry, uint32_t *entry_cluster_num, uint32_t *entry_offset_in_cluster);
static int load_fat_table(fat_fs_t *fs);
static int flush_fat_table(fat_fs_t *fs);
static uint32_t find_free_cluster(fat_fs_t *fs);
// NOTE: Using the external fat_set_cluster_entry from fat_utils.h/c instead of the conflicting static void one previously declared here.


// --- FAT Table Management Helpers ---

/* Reads a specific sector relative to the start of the FAT */
static int read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer) {
    if (!fs || !buffer) {
        return -FS_ERR_INVALID_PARAM;
    }
    uint32_t fat_start_lba = fs->boot_sector.reserved_sector_count;
    uint32_t target_lba = fat_start_lba + sector_offset;
    size_t sector_size = fs->boot_sector.bytes_per_sector;

    // Ensure sector_size is valid before using buffer_get which might rely on it internally
    if (sector_size == 0) {
        terminal_printf("[FAT] read_fat_sector: Invalid sector size (0) for device %s.\n", fs->disk.blk_dev.device_name);
        return -FS_ERR_INVALID_FORMAT;
    }

    buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
    if (!buf) {
        terminal_printf("[FAT] read_fat_sector: Failed buffer_get for LBA %u on %s.\n", target_lba, fs->disk.blk_dev.device_name);
        return -FS_ERR_IO;
    }
    memcpy(buffer, buf->data, sector_size);
    buffer_release(buf);
    return FS_SUCCESS;
}

/* Writes a specific sector relative to the start of the FAT */
static int write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer) {
     if (!fs || !buffer) {
        return -FS_ERR_INVALID_PARAM;
    }
    uint32_t fat_start_lba = fs->boot_sector.reserved_sector_count;
    uint32_t target_lba = fat_start_lba + sector_offset;
    size_t sector_size = fs->boot_sector.bytes_per_sector;

    if (sector_size == 0) {
        terminal_printf("[FAT] write_fat_sector: Invalid sector size (0) for device %s.\n", fs->disk.blk_dev.device_name);
        return -FS_ERR_INVALID_FORMAT;
    }

    buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
    if (!buf) {
        terminal_printf("[FAT] write_fat_sector: Failed buffer_get for LBA %u on %s.\n", target_lba, fs->disk.blk_dev.device_name);
        return -FS_ERR_IO;
    }
    memcpy(buf->data, buffer, sector_size);
    buffer_mark_dirty(buf); // Mark buffer dirty after writing
    buffer_release(buf);
    return FS_SUCCESS;
}

/* load_fat_table: Reads the FAT table from disk into memory using buffer cache. */
static int load_fat_table(fat_fs_t *fs) {
    if (!fs) return -FS_ERR_INVALID_PARAM;
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    if (fat_sector_count == 0 || bps == 0) {
        terminal_write("[FAT] load_fat_table: Invalid FAT size or bytes_per_sector.\n");
        return -FS_ERR_INVALID_FORMAT;
    }

    size_t table_size = fat_sector_count * bps;
    if (table_size == 0) { // Check for potential overflow if bps is huge
         terminal_write("[FAT] load_fat_table: Calculated table size is zero.\n");
         return -FS_ERR_INVALID_FORMAT;
    }

    // Allocate memory for the FAT table
    fs->fat_table = kmalloc(table_size);
    if (!fs->fat_table) {
        terminal_printf("[FAT] load_fat_table: Failed to allocate %u bytes for FAT table.\n", table_size);
        return -FS_ERR_OUT_OF_MEMORY;
    }

    // Read the FAT sectors
    uint8_t *current_ptr = (uint8_t *)fs->fat_table;
    for (uint32_t i = 0; i < fat_sector_count; i++) {
        int read_result = read_fat_sector(fs, i, current_ptr);
        if (read_result != FS_SUCCESS) {
            terminal_printf("[FAT] load_fat_table: Failed to read FAT sector %u. Error: %d\n", i, read_result);
            kfree(fs->fat_table);
            fs->fat_table = NULL;
            return -FS_ERR_IO;
        }
        current_ptr += bps;
    }
    terminal_printf("[FAT] FAT table loaded (%u sectors, %u bytes) for %s.\n", fat_sector_count, table_size, fs->disk.blk_dev.device_name);
    return FS_SUCCESS;
}

/* flush_fat_table: Writes the in‑memory FAT table back to disk using buffer cache. */
static int flush_fat_table(fat_fs_t *fs) {
    if (!fs) return -FS_ERR_INVALID_PARAM;
    if (!fs->fat_table) {
        terminal_write("[FAT] flush_fat_table: FAT table not loaded, cannot flush.\n");
        return -FS_ERR_INVALID_PARAM; // Or maybe FS_SUCCESS if nothing to flush?
    }

    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    size_t table_size = fat_sector_count * bps; // For freeing on error, if needed

    if (fat_sector_count == 0 || bps == 0 || table_size == 0) {
        terminal_write("[FAT] flush_fat_table: Invalid FAT size or bytes_per_sector.\n");
        return -FS_ERR_INVALID_FORMAT;
    }

    terminal_printf("[FAT] Flushing FAT table (%u sectors) for %s...\n", fat_sector_count, fs->disk.blk_dev.device_name);
    const uint8_t *current_ptr = (const uint8_t *)fs->fat_table;
    for (uint32_t i = 0; i < fat_sector_count; i++) {
        int write_result = write_fat_sector(fs, i, current_ptr);
        if (write_result != FS_SUCCESS) {
            terminal_printf("[FAT] flush_fat_table: Failed to write FAT sector %u. Error: %d\n", i, write_result);
            // Should we try to continue flushing other sectors? Probably not.
            return -FS_ERR_IO;
        }
        current_ptr += bps;
    }

    // Additionally, flush the buffer cache to ensure FAT sectors reach the disk
    buffer_cache_sync();

    terminal_write("[FAT] FAT table flush completed.\n");
    return FS_SUCCESS;
}

/* find_free_cluster: Scans the FAT table for a free cluster (value 0). */
static uint32_t find_free_cluster(fat_fs_t *fs) {
    if (!fs || !fs->fat_table) return 0; // Return 0 to indicate error/not found

    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_size_bytes = fs->fat_size * bps;
    uint32_t total_entries = 0;
    uint32_t *fat32 = NULL;
    uint16_t *fat16 = NULL;

    if (fs->type == FAT_TYPE_FAT32) {
        if (sizeof(uint32_t) == 0) return 0; // Avoid division by zero
        total_entries = fat_size_bytes / sizeof(uint32_t);
        fat32 = (uint32_t *)fs->fat_table;
    } else if (fs->type == FAT_TYPE_FAT16) {
        if (sizeof(uint16_t) == 0) return 0; // Avoid division by zero
        total_entries = fat_size_bytes / sizeof(uint16_t);
        fat16 = (uint16_t *)fs->fat_table;
    } else {
        terminal_write("[FAT] find_free_cluster: Unsupported FAT type.\n");
        return 0; // FAT12 or unknown
    }

    if (total_entries < 2) return 0; // Not enough entries for data clusters

    // Scan for a free entry (value 0), starting from cluster 2
    for (uint32_t i = 2; i < total_entries; i++) {
        if (fs->type == FAT_TYPE_FAT32 && (fat32[i] & 0x0FFFFFFF) == 0) return i;
        if (fs->type == FAT_TYPE_FAT16 && fat16[i] == 0) return i;
        // FAT12 check would be more complex here
    }

    terminal_write("[FAT] find_free_cluster: No free cluster found.\n");
    return 0; // Indicate not found
}


// --- VFS Driver Registration ---
static vfs_driver_t fat_vfs_driver = {
    .fs_name = "FAT", // Default name, updated in mount
    .mount   = fat_mount_internal,
    .unmount = fat_unmount_internal,
    .open    = fat_open_internal,
    .read    = fat_read_internal,
    .write   = fat_write_internal,
    .close   = fat_close_internal,
    .lseek   = NULL, // TODO: Implement lseek if needed
    .next    = NULL
};

int fat_register_driver(void) {
    terminal_write("[FAT] Registering FAT VFS driver.\n");
    return vfs_register_driver(&fat_vfs_driver);
}

void fat_unregister_driver(void) {
    terminal_write("[FAT] Unregistering FAT VFS driver.\n");
    vfs_unregister_driver(&fat_vfs_driver);
}

// --- FAT Implementation Functions ---
static void *fat_mount_internal(const char *device) {
     if (!device) {
         terminal_write("[FAT] mount_internal: Invalid device name (NULL).\n");
         return NULL;
     }
     terminal_printf("[FAT] Attempting to mount device '%s'...\n", device);

     fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
     if (!fs) {
         terminal_write("[FAT] mount_internal: Failed to allocate memory for fat_fs_t.\n");
         return NULL;
     }
     memset(fs, 0, sizeof(fat_fs_t));

     // Initialize underlying disk
     if (disk_init(&fs->disk, device) != 0) {
         terminal_printf("[FAT] mount_internal: Failed to initialize disk '%s'.\n", device);
         kfree(fs);
         return NULL;
     }

     // Basic validation of underlying block device info
     if (fs->disk.blk_dev.sector_size == 0) {
         terminal_printf("[FAT] mount_internal: Disk '%s' reported sector size 0.\n", device);
          kfree(fs);
         return NULL;
     }

     // Read boot sector (Sector 0) using buffer cache
     buffer_t *bs_buf = buffer_get(device, 0);
     if (!bs_buf) {
          terminal_printf("[FAT] mount_internal: Failed to read boot sector (LBA 0) for '%s'.\n", device);
         kfree(fs);
         return NULL;
     }
     memcpy(&fs->boot_sector, bs_buf->data, sizeof(fat_boot_sector_t));
     buffer_release(bs_buf); // Release buffer immediately after copy

     // Validate boot sector signature
     if (fs->boot_sector.boot_sector_signature != 0xAA55) {
         terminal_printf("[FAT] mount_internal: Invalid boot sector signature (0x%x) on '%s'.\n", fs->boot_sector.boot_sector_signature, device);
         kfree(fs);
         return NULL;
     }

     // Validate bytes per sector
     if (fs->boot_sector.bytes_per_sector == 0) {
          terminal_printf("[FAT] mount_internal: Invalid bytes_per_sector (0) in boot sector on '%s'.\n", device);
         kfree(fs);
         return NULL;
     }
     // Update underlying block device's sector size if different? Or trust IDENTIFY?
     // Let's trust the boot sector for FAT geometry.
     fs->disk.blk_dev.sector_size = fs->boot_sector.bytes_per_sector;
     size_t sector_size = fs->disk.blk_dev.sector_size;

     // Calculate filesystem parameters
     uint32_t total_sectors = (fs->boot_sector.total_sectors_short != 0) ? fs->boot_sector.total_sectors_short : fs->boot_sector.total_sectors_long;
     uint32_t fat_size = (fs->boot_sector.fat_size_16 != 0) ? fs->boot_sector.fat_size_16 : fs->boot_sector.fat_size_32;
     if (total_sectors == 0 || fat_size == 0 || fs->boot_sector.num_fats == 0) {
         terminal_printf("[FAT] mount_internal: Invalid geometry (TotalSectors=%u, FatSize=%u, NumFATs=%u) on '%s'.\n",
                         total_sectors, fat_size, fs->boot_sector.num_fats, device);
         kfree(fs); return NULL;
     }
     fs->total_sectors = total_sectors;
     fs->fat_size = fat_size; // Size of ONE fat in sectors

     // Calculate root directory sectors (only relevant for FAT12/16)
     uint32_t root_dir_sectors = 0;
     if (fs->boot_sector.root_entry_count != 0) { // Check needed for FAT32
        root_dir_sectors = ((fs->boot_sector.root_entry_count * sizeof(fat_dir_entry_t)) + (sector_size - 1)) / sector_size;
     }
     fs->root_dir_sectors = root_dir_sectors;

     // Calculate first data sector
     fs->first_data_sector = fs->boot_sector.reserved_sector_count + (fs->boot_sector.num_fats * fs->fat_size) + root_dir_sectors;
     if (fs->first_data_sector >= total_sectors) {
          terminal_printf("[FAT] mount_internal: Calculated first data sector (%u) exceeds total sectors (%u) on '%s'.\n",
                         fs->first_data_sector, total_sectors, device);
          kfree(fs); return NULL;
     }

     // Calculate data sectors and cluster count
     if (fs->boot_sector.sectors_per_cluster == 0) {
          terminal_printf("[FAT] mount_internal: Invalid sectors_per_cluster (0) in boot sector on '%s'.\n", device);
          kfree(fs); return NULL;
     }
     uint32_t data_sectors = total_sectors - fs->first_data_sector;
     fs->cluster_count = data_sectors / fs->boot_sector.sectors_per_cluster;

     // Determine FAT type based on cluster count
     if (fs->cluster_count < 4085) {
         fs->type = FAT_TYPE_FAT12;
         fat_vfs_driver.fs_name = "FAT12";
         terminal_write("  Detected FAT12.\n");
     } else if (fs->cluster_count < 65525) {
         fs->type = FAT_TYPE_FAT16;
         fat_vfs_driver.fs_name = "FAT16";
         terminal_write("  Detected FAT16.\n");
     } else {
         fs->type = FAT_TYPE_FAT32;
         fs->root_dir_sectors = 0; // Root dir is clustered in FAT32
         fat_vfs_driver.fs_name = "FAT32";
          terminal_write("  Detected FAT32.\n");
     }

     // FAT12 is not fully supported by write/alloc operations yet
     if (fs->type == FAT_TYPE_FAT12) {
         terminal_write("  [WARNING] FAT12 support is limited (read-only recommended).\n");
     }

     // --- FIX: Removed incorrect assignment ---
     // fs->device = device; // Incorrect: fat_fs_t has no 'device' member; use fs->disk

     // Load FAT table into memory
     if (load_fat_table(fs) != FS_SUCCESS) {
         terminal_printf("[FAT] mount_internal: Failed to load FAT table for '%s'.\n", device);
         kfree(fs);
         return NULL;
     }

     terminal_printf("[FAT] Successfully mounted '%s' as %s. Context: 0x%x\n", device, fat_vfs_driver.fs_name, (uintptr_t)fs);
     return fs; // Return the filesystem context
}

static int fat_unmount_internal(void *fs_context) {
    fat_fs_t *fs = (fat_fs_t *)fs_context;
    if (!fs) {
        terminal_write("[FAT] unmount_internal: Invalid context (NULL).\n");
        return -FS_ERR_INVALID_PARAM;
    }
    terminal_printf("[FAT] Unmounting %s (%s)...\n", fs->disk.blk_dev.device_name, fat_vfs_driver.fs_name);

    // Flush and free the FAT table if loaded
    if (fs->fat_table) {
        if (flush_fat_table(fs) != FS_SUCCESS) {
            terminal_printf("[FAT] unmount_internal: Warning - failed to flush FAT table for %s.\n", fs->disk.blk_dev.device_name);
            // Continue unmount despite flush failure? Yes.
        }
        size_t table_size = fs->fat_size * fs->boot_sector.bytes_per_sector;
        if (table_size > 0) { // Avoid kfree with size 0
             kfree(fs->fat_table);
        }
        fs->fat_table = NULL;
    }

    // Sync any remaining dirty buffers for this device (important!)
    // Note: buffer_cache_sync() syncs all buffers. A targeted sync might be better.
    buffer_cache_sync();

    // Free the filesystem context structure
    kfree(fs);
    terminal_write("[FAT] Unmount complete.\n");
    return FS_SUCCESS;
}

// --- FAT File Context ---
// Holds state for an open file within the FAT filesystem
typedef struct {
    fat_fs_t *fs;                   // Pointer back to the filesystem instance
    uint32_t first_cluster;         // First cluster of the file/directory data
    uint32_t current_cluster;       // Current cluster being accessed for read/write
    uint32_t file_size;             // Size of the file in bytes
    uint32_t dir_entry_cluster;     // Cluster containing the directory entry for this file (0 for FAT16 root)
    uint32_t dir_entry_offset;      // Byte offset of the entry within its cluster/root area
    bool dirty;                     // Flag: true if file metadata (size, etc.) changed and needs update on close
} fat_file_context_t;

// --- Cluster Read/Write Helpers ---
static int read_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, void *buf, size_t len) {
    if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    if (lba == 0) { // fat_cluster_to_lba returns 0 on error
        terminal_printf("[FAT] read_cluster: Invalid LBA for cluster %u.\n", cluster);
        return -FS_ERR_INVALID_PARAM;
    }

    size_t sector_size = fs->boot_sector.bytes_per_sector;
    size_t sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
    size_t cluster_size = sector_size * sectors_per_cluster;

    if (cluster_size == 0 || sector_size == 0) return -FS_ERR_INVALID_FORMAT;
    if (offset_in_cluster >= cluster_size) return -FS_ERR_INVALID_PARAM; // Offset out of bounds

    size_t bytes_read = 0;
    uint8_t *out_buf = (uint8_t *)buf;

    while (bytes_read < len) {
        uint32_t current_offset_in_cluster = offset_in_cluster + bytes_read;
        if (current_offset_in_cluster >= cluster_size) break; // Read past end of cluster

        uint32_t sector_index = current_offset_in_cluster / sector_size;
        uint32_t offset_in_sector = current_offset_in_cluster % sector_size;
        size_t read_len = sector_size - offset_in_sector; // Max read from this sector

        if (read_len > (len - bytes_read)) { // Don't read more than requested
            read_len = len - bytes_read;
        }
        if (sector_index >= sectors_per_cluster) break; // Should not happen if offset_in_cluster check passed

        buffer_t *b = buffer_get(fs->disk.blk_dev.device_name, lba + sector_index);
        if (!b) return -FS_ERR_IO;

        memcpy(out_buf + bytes_read, b->data + offset_in_sector, read_len);
        buffer_release(b);
        bytes_read += read_len;
    }
    return (int)bytes_read;
}

static int write_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, const void *buf, size_t len) {
    if (!fs || !buf || cluster < 2) return -FS_ERR_INVALID_PARAM;

    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    if (lba == 0) {
        terminal_printf("[FAT] write_cluster: Invalid LBA for cluster %u.\n", cluster);
        return -FS_ERR_INVALID_PARAM;
    }

    size_t sector_size = fs->boot_sector.bytes_per_sector;
    size_t sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
    size_t cluster_size = sector_size * sectors_per_cluster;

    if (cluster_size == 0 || sector_size == 0) return -FS_ERR_INVALID_FORMAT;
    if (offset_in_cluster >= cluster_size) return -FS_ERR_INVALID_PARAM; // Offset out of bounds

    size_t bytes_written = 0;
    const uint8_t *in_buf = (const uint8_t *)buf;

    while (bytes_written < len) {
        uint32_t current_offset_in_cluster = offset_in_cluster + bytes_written;
        if (current_offset_in_cluster >= cluster_size) break; // Write past end of cluster

        uint32_t sector_index = current_offset_in_cluster / sector_size;
        uint32_t offset_in_sector = current_offset_in_cluster % sector_size;
        size_t write_len = sector_size - offset_in_sector; // Max write to this sector

        if (write_len > (len - bytes_written)) { // Don't write more than provided
            write_len = len - bytes_written;
        }
        if (sector_index >= sectors_per_cluster) break; // Safety check

        // Get buffer (read content first, even if overwriting)
        buffer_t *b = buffer_get(fs->disk.blk_dev.device_name, lba + sector_index);
        if (!b) return -FS_ERR_IO;

        memcpy(b->data + offset_in_sector, in_buf + bytes_written, write_len);
        buffer_mark_dirty(b); // Mark dirty after modifying
        buffer_release(b);
        bytes_written += write_len;
    }
    return (int)bytes_written;
}

/**
 * find_directory_entry: Finds a directory entry in a given directory cluster chain.
 * Handles FAT32 root directory (starting at boot_sector.root_cluster) and subdirectories.
 * Does NOT handle FAT12/16 fixed root directory area yet.
 * Does NOT handle LFNs (Long File Names).
 * Does NOT handle path splitting (expects only the final component name).
 *
 * @param fs Filesystem context.
 * @param dir_start_cluster Starting cluster of the directory to search.
 * @param name_8_3 The 8.3 formatted filename (11 bytes, no null) to find.
 * @param entry Output pointer to store the found entry.
 * @param entry_cluster_num Output pointer to store the cluster where the entry was found.
 * @param entry_offset_in_cluster Output pointer to store the byte offset within the cluster.
 * @return FS_SUCCESS on success, FS_ERR_NOT_FOUND, or other FS_ERR_* code.
 */
static int find_entry_in_dir(fat_fs_t *fs, uint32_t dir_start_cluster, const char name_8_3[11], fat_dir_entry_t *entry, uint32_t *entry_cluster_num, uint32_t *entry_offset_in_cluster) {
    if (!fs || !name_8_3 || !entry || !entry_cluster_num || !entry_offset_in_cluster) {
        return -FS_ERR_INVALID_PARAM;
    }
    // Basic check for FAT32 root or valid cluster
    if (fs->type != FAT_TYPE_FAT32 && dir_start_cluster == 0) {
        terminal_write("[FAT] find_entry_in_dir: FAT12/16 fixed root search not implemented.\n");
        return -FS_ERR_NOT_SUPPORTED;
    }
    if (dir_start_cluster < 2 && !(fs->type == FAT_TYPE_FAT32 && dir_start_cluster == fs->boot_sector.root_cluster)) {
         terminal_printf("[FAT] find_entry_in_dir: Invalid start cluster %u.\n", dir_start_cluster);
         return -FS_ERR_INVALID_PARAM;
    }

    size_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
    size_t entries_per_cluster = cluster_size / sizeof(fat_dir_entry_t);
    if (entries_per_cluster == 0) return -FS_ERR_INVALID_FORMAT;

    uint8_t* cluster_buffer = (uint8_t*)kmalloc(cluster_size);
    if (!cluster_buffer) return -FS_ERR_OUT_OF_MEMORY;

    uint32_t current_cluster = dir_start_cluster;
    uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFF8 : 0x0FFFFFF8; // Use EOC start marker

    while (current_cluster >= 2 && current_cluster < eoc_marker) {
        int read_res = read_cluster(fs, current_cluster, 0, cluster_buffer, cluster_size);
        if (read_res < 0) {
            kfree(cluster_buffer);
            return read_res; // Propagate read error
        }
        if ((size_t)read_res != cluster_size && read_res > 0) {
            // Partial read? This indicates an issue.
            terminal_printf("[FAT] find_entry_in_dir: Warning - partial read (%d/%u bytes) for cluster %u.\n", read_res, cluster_size, current_cluster);
            // Clear the rest of the buffer to avoid reading garbage?
            memset(cluster_buffer + read_res, 0, cluster_size - read_res);
        }

        fat_dir_entry_t *entries = (fat_dir_entry_t *)cluster_buffer;
        for (size_t i = 0; i < entries_per_cluster; ++i) {
            uint8_t first_byte = entries[i].name[0];

            if (first_byte == 0x00) { // End of directory marker
                kfree(cluster_buffer);
                return -FS_ERR_NOT_FOUND;
            }
            if (first_byte == 0xE5) { // Deleted entry marker
                continue;
            }
            // Skip volume label entries and LFN entries
            if ((entries[i].attr & 0x08 /* ATTR_VOLUME_ID */) || (entries[i].attr == 0x0F /* ATTR_LONG_NAME */)) {
                continue;
            }

            // Compare 8.3 names
            if (memcmp(entries[i].name, name_8_3, 11) == 0) {
                memcpy(entry, &entries[i], sizeof(fat_dir_entry_t));
                *entry_cluster_num = current_cluster;
                *entry_offset_in_cluster = i * sizeof(fat_dir_entry_t);
                kfree(cluster_buffer);
                return FS_SUCCESS; // Found it!
            }
        } // End loop through entries in cluster

        // Get next cluster in the directory chain
        uint32_t next_cluster = 0;
        if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) {
            kfree(cluster_buffer);
            return -FS_ERR_IO; // Error reading FAT
        }
        current_cluster = next_cluster;
    } // End loop through cluster chain

    kfree(cluster_buffer);
    return -FS_ERR_NOT_FOUND; // Reached end of chain without finding
}


/**
 * find_directory_entry: Finds a directory entry by path.
 * Currently only supports simple paths within the root directory for FAT32.
 * Needs enhancement for subdirectories and FAT12/16 root.
 */
static int find_directory_entry(fat_fs_t *fs, const char *path, fat_dir_entry_t *entry, uint32_t *entry_cluster_num, uint32_t *entry_offset_in_cluster) {
    if (!fs || !path || !entry || !entry_cluster_num || !entry_offset_in_cluster) {
        return -FS_ERR_INVALID_PARAM;
    }
    terminal_printf("[FAT DEBUG] find_directory_entry: path='%s'\n", path);

    // --- Basic Path Handling (Needs Improvement) ---
    // This simplistic version assumes path is just a filename in the root dir.
    // A real implementation needs to parse path components like "/"
    // and traverse subdirectories.
    if (strchr(path, '/') != NULL && !(path[0] == '/' && path[1] == '\0')) {
        terminal_write("  [ERROR] Subdirectory paths not yet supported by find_directory_entry.\n");
        return -FS_ERR_NOT_SUPPORTED;
    }
    // Handle root path "/" case?
    if (strcmp(path, "/") == 0) {
        terminal_write("  [ERROR] Searching for root '/' itself is not supported here.\n");
        return -FS_ERR_IS_A_DIRECTORY; // Or invalid param?
    }

    // Extract the base filename component
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path; // Get pointer after last '/' or start of string
    if (strlen(basename) == 0) { // e.g., path ends in "/"
        return -FS_ERR_INVALID_PARAM;
    }

    // Convert to 8.3 format
    char fat_filename[11];
    format_filename(basename, fat_filename);
    terminal_printf("  Searching for 8.3 name: '%.11s'\n", fat_filename);

    // --- Determine Starting Directory Cluster ---
    uint32_t start_cluster;
    if (fs->type == FAT_TYPE_FAT32) {
        start_cluster = fs->boot_sector.root_cluster;
        terminal_printf("  Starting search in FAT32 root cluster: %u\n", start_cluster);
    } else {
        // FAT12/16 fixed root area search is not implemented here yet.
        terminal_write("  [ERROR] FAT12/16 root directory search not implemented.\n");
        return -FS_ERR_NOT_SUPPORTED;
    }

    // --- Search the Directory ---
    return find_entry_in_dir(fs, start_cluster, fat_filename, entry, entry_cluster_num, entry_offset_in_cluster);
}


/**
 * fat_open_internal: Opens a file/directory via VFS.
 */
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags) {
    fat_fs_t *fs = (fat_fs_t *)fs_context;
    if (!fs || !path) {
        terminal_write("[FAT] open_internal: Invalid context or path.\n");
        return NULL;
    }
    terminal_printf("[FAT] open_internal: path='%s', flags=0x%x\n", path, flags);

    // Find the directory entry
    fat_dir_entry_t entry;
    uint32_t dir_cluster, dir_offset;
    int find_res = find_directory_entry(fs, path, &entry, &dir_cluster, &dir_offset);

    // TODO: Handle O_CREAT flag - If file not found and O_CREAT is set, need to:
    // 1. Find a free slot in the parent directory.
    // 2. Find a free cluster for the file data (if not zero size).
    // 3. Update FAT for the new cluster (mark as EOC).
    // 4. Write the new directory entry.
    // 5. Flush FAT and directory buffer.
    if (find_res == -FS_ERR_NOT_FOUND && (flags & O_CREAT)) {
         terminal_printf("[FAT] open_internal: TODO - Handle O_CREAT for path '%s'.\n", path);
         return NULL; // Creation not implemented yet
    } else if (find_res != FS_SUCCESS) {
         terminal_printf("[FAT] open_internal: Failed to find entry for '%s'. Error: %d\n", path, find_res);
        return NULL; // Other error or not found without O_CREAT
    }

    // Cannot open directories with O_WRONLY or O_RDWR yet (no writing support for dirs)
    if ((entry.attr & 0x10 /* ATTR_DIRECTORY */) && (flags & (O_WRONLY | O_RDWR))) {
        terminal_printf("[FAT] open_internal: Cannot open directory '%s' for writing.\n", path);
        return NULL;
    }

    // TODO: Handle O_TRUNC flag - If file exists and O_TRUNC is set:
    // 1. Free all clusters allocated to the file (traverse FAT chain and set entries to 0).
    // 2. Update directory entry: set file size to 0, first cluster to 0.
    // 3. Flush FAT and directory buffer.
    if (!(entry.attr & 0x10) && (flags & O_TRUNC)) {
        terminal_printf("[FAT] open_internal: TODO - Handle O_TRUNC for file '%s'.\n", path);
        // For now, just proceed without truncating
    }


    terminal_write("  Entry found. Allocating vnode/context...\n");
    vnode_t *vnode = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vnode) {
        terminal_write("  [ERROR] Failed kmalloc for vnode.\n");
        return NULL;
    }
    memset(vnode, 0, sizeof(vnode_t)); // Initialize vnode

    fat_file_context_t *file_ctx = (fat_file_context_t *)kmalloc(sizeof(fat_file_context_t));
     if (!file_ctx) {
        terminal_write("  [ERROR] Failed kmalloc for file context.\n");
        kfree(vnode);
        return NULL;
    }
    memset(file_ctx, 0, sizeof(fat_file_context_t)); // Initialize context


    // Populate file context
    file_ctx->fs = fs;
    file_ctx->first_cluster = (((uint32_t)entry.first_cluster_high) << 16) | entry.first_cluster_low;
    file_ctx->current_cluster = file_ctx->first_cluster; // Start at the beginning
    file_ctx->file_size = entry.file_size;
    file_ctx->dir_entry_cluster = dir_cluster; // Store location for potential updates
    file_ctx->dir_entry_offset = dir_offset;
    file_ctx->dirty = false; // Not modified yet

    // Populate vnode
    vnode->data = file_ctx;
    vnode->fs_driver = &fat_vfs_driver;
    // TODO: Populate other vnode fields if added (type, size, etc.)

    terminal_printf("  Returning vnode: 0x%x (context: 0x%x)\n", (uintptr_t)vnode, (uintptr_t)file_ctx);
    return vnode;
}

/**
 * fat_read_internal: Reads data from an open file.
 */
static int fat_read_internal(file_t *file, void *buf, size_t len) {
    if (!file || !file->vnode || !file->vnode->data || !buf) return -FS_ERR_INVALID_PARAM;
    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    if (!fctx || !fctx->fs) return -FS_ERR_INVALID_PARAM; // Check nested pointers

    fat_fs_t *fs = fctx->fs;
    size_t total_read = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;

    // --- Calculate actual read length ---
    if (file->offset < 0) file->offset = 0; // Cannot read before start
    if ((uint64_t)file->offset >= (uint64_t)fctx->file_size) {
         return 0; // Read starting at or past EOF
    }
    size_t remaining_in_file = fctx->file_size - (size_t)file->offset;
    if (len > remaining_in_file) {
        len = remaining_in_file; // Clamp read length to file end
    }
    if (len == 0) return 0; // Nothing to read

    // --- Find starting cluster and offset ---
    uint32_t current_cluster = fctx->first_cluster;
    uint32_t target_cluster_index = (uint32_t)(file->offset / cluster_size);
    uint32_t offset_in_cluster = (uint32_t)(file->offset % cluster_size);

    // Check if file has allocated clusters
    if (current_cluster < 2) {
        // File has size > 0 but no cluster? Indicates corruption or zero-size file handled incorrectly.
        if (fctx->file_size > 0) {
             terminal_printf("[FAT] read_internal: File size %u but first cluster is %u.\n", fctx->file_size, current_cluster);
             return -FS_ERR_CORRUPT;
        }
        return 0; // Zero size file, nothing to read
    }


    // --- Traverse FAT chain to the starting cluster ---
    uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFF8 : 0x0FFFFFF8; // EOC start marker
    for (uint32_t i = 0; i < target_cluster_index; ++i) {
         uint32_t next_cluster = 0;
         if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) return -FS_ERR_IO;
         if (next_cluster < 2 || next_cluster >= eoc_marker) {
              // Reached EOC prematurely before target cluster index
              terminal_printf("[FAT] read_internal: Premature EOC at cluster %u while seeking index %u (offset %ld).\n", current_cluster, target_cluster_index, file->offset);
              return total_read > 0 ? (int)total_read : -FS_ERR_CORRUPT;
         }
         current_cluster = next_cluster;
    }
    // Now, current_cluster holds the cluster containing the start of our read


    // --- Read loop across clusters ---
    while (total_read < len) {
        if (current_cluster < 2 || current_cluster >= eoc_marker) {
             terminal_printf("[FAT] read_internal: Hit EOC marker (0x%x) unexpectedly during read loop.\n", current_cluster);
             break; // EOC reached
        }

        size_t bytes_to_read_from_cluster = cluster_size - offset_in_cluster;
        if (bytes_to_read_from_cluster > (len - total_read)) {
            bytes_to_read_from_cluster = len - total_read;
        }

        int cluster_read_result = read_cluster(fs, current_cluster, offset_in_cluster, (uint8_t *)buf + total_read, bytes_to_read_from_cluster);

        if (cluster_read_result < 0) return cluster_read_result; // Propagate read error
        if (cluster_read_result == 0 && bytes_to_read_from_cluster > 0) {
             terminal_printf("[FAT] read_internal: read_cluster returned 0 unexpectedly for cluster %u.\n", current_cluster);
             break; // EOF within cluster? or error?
        }

        total_read += cluster_read_result;
        file->offset += cluster_read_result; // Update the file handle's offset

        // --- Move to next cluster if necessary ---
        if (total_read < len) { // Only need next cluster if more data is required
            offset_in_cluster = (offset_in_cluster + cluster_read_result) % cluster_size; // Update offset, wrap to 0 if full cluster read
            if (offset_in_cluster == 0) { // We finished reading the current cluster
                uint32_t next_cluster = 0;
                if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) {
                     terminal_printf("[FAT] read_internal: Error reading FAT for next cluster after %u.\n", current_cluster);
                     return -FS_ERR_IO;
                }
                current_cluster = next_cluster; // Move to the next cluster
            }
        }
    } // End while loop

    // Update the file context's current cluster (maybe useful for subsequent reads?)
    fctx->current_cluster = current_cluster;

    return (int)total_read;
}


/**
 * fat_write_internal: Writes data to an open file.
 * Basic implementation supports appending or overwriting within the current size.
 * Needs cluster allocation for extending files.
 */
static int fat_write_internal(file_t *file, const void *buf, size_t len) {
    if (!file || !file->vnode || !file->vnode->data || !buf) return -FS_ERR_INVALID_PARAM;
    if (len == 0) return 0;

    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
     if (!fctx || !fctx->fs) return -FS_ERR_INVALID_PARAM; // Check nested pointers

    // Check if opened for writing
    if (!(file->flags & (O_WRONLY | O_RDWR))) {
        return -FS_ERR_PERMISSION_DENIED; // Or EBADF
    }
    // Check if it's a directory (directories cannot be written to like files)
    // Need to read the dir entry attributes if not stored in vnode/fctx
    // fat_dir_entry_t temp_entry;
    // if (read_directory_entry(fctx->fs, fctx->dir_entry_cluster, fctx->dir_entry_offset, &temp_entry) == FS_SUCCESS) {
    //    if (temp_entry.attr & 0x10 /* ATTR_DIRECTORY */) return -FS_ERR_IS_A_DIRECTORY;
    // } else { return -FS_ERR_IO; } // Error reading entry


    fat_fs_t *fs = fctx->fs;
    size_t total_written = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;

    // Ensure FAT table is loaded (might be needed for allocation)
    if (!fs->fat_table) {
        if(load_fat_table(fs) != FS_SUCCESS) return -FS_ERR_IO;
    }

    // --- Handle O_APPEND flag ---
    if (file->flags & O_APPEND) {
        file->offset = (off_t)fctx->file_size; // Seek to end before writing
    }
    if (file->offset < 0) file->offset = 0; // Safety check

    // Determine EOC marker based on FAT type
    uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFFF : 0x0FFFFFFF; // Use actual EOC values

    // --- Find starting cluster and offset ---
    uint32_t current_cluster = fctx->first_cluster;
    uint32_t target_cluster_index = (file->offset == 0) ? 0 : (uint32_t)(file->offset / cluster_size); // Handle offset 0 case
    uint32_t offset_in_cluster = (uint32_t)(file->offset % cluster_size);

    // --- Allocate first cluster if file is currently empty ---
    if (current_cluster < 2 && fctx->file_size == 0 && file->offset == 0) {
        terminal_printf("[FAT write] Allocating first cluster for PID (offset %ld)\n", file->offset);
        uint32_t free_cluster = find_free_cluster(fs);
        if (free_cluster == 0) return -FS_ERR_NO_SPACE;
        if (fat_set_cluster_entry(fs, free_cluster, eoc_marker) != FS_SUCCESS) return -FS_ERR_IO;

        fctx->first_cluster = current_cluster = free_cluster;
        fctx->dirty = true; // Mark context dirty to update dir entry later
        // The directory entry's first cluster field needs update in fat_close
        if (flush_fat_table(fs)!=FS_SUCCESS) return -FS_ERR_IO; // Flush FAT change immediately after alloc
        terminal_printf("  Allocated cluster %u as first cluster.\n", current_cluster);

    } else if (current_cluster < 2 && file->offset > 0) {
         // Trying to write past end of an empty file without allocation - error
         terminal_printf("[FAT write] Error: Cannot seek then write to empty file (offset %ld).\n", file->offset);
         return -FS_ERR_INVALID_PARAM;
    }


    // --- Traverse or extend FAT chain to the starting cluster ---
    // Need to handle extending the file if offset is beyond current size
    uint32_t last_valid_cluster_index = (fctx->file_size == 0) ? (uint32_t)-1 : (fctx->file_size - 1) / cluster_size;
    uint32_t current_cluster_index = 0;

    // Traverse only if we need to reach a cluster > 0 and the file isn't empty
    if (target_cluster_index > 0 && current_cluster >= 2) {
        uint32_t prev_cluster = current_cluster; // Keep track for allocation
        while (current_cluster_index < target_cluster_index) {
            uint32_t next_cluster = 0;
            if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) return -FS_ERR_IO;

            if (next_cluster < 2 || next_cluster >= eoc_marker) { // End of chain reached
                 if (current_cluster_index == last_valid_cluster_index) {
                     // We are at the last allocated cluster, need to extend
                     terminal_printf("[FAT write] Extending file: end of chain at cluster %u (index %u), target index %u.\n", current_cluster, current_cluster_index, target_cluster_index);
                     uint32_t free_cluster = find_free_cluster(fs);
                     if (free_cluster == 0) { flush_fat_table(fs); return total_written > 0 ? (int)total_written : -FS_ERR_NO_SPACE; }
                     if (fat_set_cluster_entry(fs, current_cluster, free_cluster) != FS_SUCCESS) { flush_fat_table(fs); return -FS_ERR_IO; }
                     if (fat_set_cluster_entry(fs, free_cluster, eoc_marker) != FS_SUCCESS) { flush_fat_table(fs); return -FS_ERR_IO; }
                     fctx->dirty = true;
                     if (flush_fat_table(fs)!=FS_SUCCESS) return -FS_ERR_IO; // Flush FAT changes
                     next_cluster = free_cluster;
                     terminal_printf("  Allocated new cluster %u.\n", next_cluster);
                 } else {
                     // EOC reached before the file's logical end or target offset? Corruption.
                     terminal_printf("[FAT write] Error: Premature EOC at cluster %u (index %u) file size %u, target index %u.\n", current_cluster, current_cluster_index, fctx->file_size, target_cluster_index);
                     return -FS_ERR_CORRUPT;
                 }
            }
            prev_cluster = current_cluster;
            current_cluster = next_cluster;
            current_cluster_index++;
        } // end while traverse
    } // end if target_cluster_index > 0

    // At this point, current_cluster should be the cluster where writing begins.
    fctx->current_cluster = current_cluster; // Store for potential subsequent writes

    // --- Write loop across clusters ---
    while (total_written < len) {
        if (current_cluster < 2) {
            terminal_printf("[FAT write] Error: Invalid current_cluster (%u) in write loop.\n", current_cluster);
            return -FS_ERR_CORRUPT; // Should have been allocated
        }

        size_t bytes_to_write_in_cluster = cluster_size - offset_in_cluster;
        if (bytes_to_write_in_cluster > (len - total_written)) {
            bytes_to_write_in_cluster = len - total_written;
        }

        int cluster_write_result = write_cluster(fs, current_cluster, offset_in_cluster, (const uint8_t *)buf + total_written, bytes_to_write_in_cluster);
        if (cluster_write_result < 0) { flush_fat_table(fs); return cluster_write_result; } // Propagate error
        if ((size_t)cluster_write_result != bytes_to_write_in_cluster) {
             // Handle short write? This might indicate a disk error not caught by lower layers.
             terminal_printf("[FAT write] Warning: Short write to cluster %u (wrote %d, expected %u).\n", current_cluster, cluster_write_result, bytes_to_write_in_cluster);
             total_written += cluster_write_result;
             file->offset += cluster_write_result;
             fctx->dirty = true;
             break; // Stop writing on short write
        }

        total_written += bytes_to_write_in_cluster;
        file->offset += bytes_to_write_in_cluster; // Update file handle's offset
        fctx->dirty = true; // Mark context dirty on successful write

        // Update file size in context if we wrote past the previous end
        if ((uint64_t)file->offset > (uint64_t)fctx->file_size) {
            fctx->file_size = (uint32_t)file->offset;
        }

        // --- Allocate next cluster if needed ---
        if (total_written < len) { // Only need next cluster if more data needs writing
             offset_in_cluster = (offset_in_cluster + cluster_write_result) % cluster_size;
             if (offset_in_cluster == 0) { // Finished writing current cluster
                  uint32_t next_cluster = 0;
                  fat_get_next_cluster(fs, current_cluster, &next_cluster); // Check current FAT entry
                  if (next_cluster < 2 || next_cluster >= eoc_marker) { // End of chain, need to allocate new
                       terminal_printf("[FAT write] Allocating next cluster after %u...\n", current_cluster);
                       uint32_t free_cluster = find_free_cluster(fs);
                       if (free_cluster == 0) { flush_fat_table(fs); return total_written > 0 ? (int)total_written : -FS_ERR_NO_SPACE; }
                       if (fat_set_cluster_entry(fs, current_cluster, free_cluster) != FS_SUCCESS) { flush_fat_table(fs); return -FS_ERR_IO; }
                       if (fat_set_cluster_entry(fs, free_cluster, eoc_marker) != FS_SUCCESS) { flush_fat_table(fs); return -FS_ERR_IO; }
                       if (flush_fat_table(fs)!=FS_SUCCESS) return -FS_ERR_IO; // Flush FAT changes
                       next_cluster = free_cluster;
                       terminal_printf("  Allocated new cluster %u.\n", next_cluster);
                  }
                  current_cluster = next_cluster;
                  fctx->current_cluster = current_cluster; // Update context
             } // if offset_in_cluster == 0
        } // if total_written < len
    } // End while loop

    // Flush FAT table at the end of the operation (might have been flushed during alloc already)
    flush_fat_table(fs);
    return (int)total_written;
}

/**
 * fat_close_internal: Closes the file, updating the directory entry if modified.
 */
static int fat_close_internal(file_t *file) {
    if (!file || !file->vnode || !file->vnode->data) return -FS_ERR_INVALID_PARAM;
    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
     if (!fctx || !fctx->fs) return -FS_ERR_INVALID_PARAM; // Check nested pointers
    fat_fs_t *fs = fctx->fs;

    if (fctx->dirty) {
        terminal_printf("[FAT] close_internal: File is dirty (PID %d?). Updating directory entry...\n", fctx->fs->disk.blk_dev.device_name); // Using device name as proxy for which FS instance
        terminal_printf("  Entry was at Cluster: %u, Offset: %u\n", fctx->dir_entry_cluster, fctx->dir_entry_offset);
        terminal_printf("  New Size: %u, First Cluster: %u\n", fctx->file_size, fctx->first_cluster);

        // --- Update Directory Entry ---
        size_t sector_size = fs->boot_sector.bytes_per_sector;
        uint32_t lba;
        size_t offset_in_sector;

        // Calculate LBA and offset within that LBA sector for the directory entry
        if (fctx->dir_entry_cluster == 0) { // FAT12/16 Root Directory (Requires Implementation)
            terminal_write("  [ERROR] Updating FAT12/16 root directory entries not implemented in fat_close!\n");
            // Need to calculate LBA based on reserved sectors, FAT size, and entry offset
            // uint32_t root_dir_start_sector = fs->boot_sector.reserved_sector_count + (fs->boot_sector.num_fats * fs->fat_size);
            // lba = root_dir_start_sector + (fctx->dir_entry_offset / sector_size);
            // offset_in_sector = fctx->dir_entry_offset % sector_size;
            // If implementing, ensure you handle reads/writes spanning multiple root sectors.
            lba = 0; offset_in_sector = 0; // Prevent use of uninitialized vars
        } else { // FAT32 Directory Cluster (or FAT12/16 subdirectory)
            uint32_t sector_in_cluster = fctx->dir_entry_offset / sector_size;
            offset_in_sector = fctx->dir_entry_offset % sector_size;
            lba = fat_cluster_to_lba(fs, fctx->dir_entry_cluster);
            if (lba == 0) {
                 terminal_printf("  [ERROR] Invalid LBA for directory cluster %u.\n", fctx->dir_entry_cluster);
                 // Cannot update entry, but still need to free memory
                 goto cleanup;
            }
            lba += sector_in_cluster;
        }

        // Read the directory sector using buffer cache
        terminal_printf("  Updating entry at LBA %u, offset_in_sector %u\n", lba, (uint32_t)offset_in_sector);
        buffer_t *dir_buf = buffer_get(fs->disk.blk_dev.device_name, lba);
        if (!dir_buf) {
            terminal_printf("  [ERROR] Failed to get buffer for directory entry sector LBA %u!\n", lba);
            // Continue with close, but metadata update failed
        } else {
             // Modify the entry in the buffer
             fat_dir_entry_t *entry_in_buf = (fat_dir_entry_t *)(dir_buf->data + offset_in_sector);

             entry_in_buf->file_size = fctx->file_size;
             entry_in_buf->first_cluster_low = (uint16_t)(fctx->first_cluster & 0xFFFF);
             entry_in_buf->first_cluster_high = (uint16_t)((fctx->first_cluster >> 16) & 0xFFFF);
             // TODO: Update timestamps (write_time, write_date)
             // Need a source of current time/date.
             // entry_in_buf->write_time = ...;
             // entry_in_buf->write_date = ...;

             // Mark buffer dirty and release
             buffer_mark_dirty(dir_buf);
             buffer_release(dir_buf);
             terminal_write("  Directory entry buffer marked dirty.\n");

             // Ensure FAT and the directory buffer are flushed
             flush_fat_table(fs); // Flush any FAT changes (redundant if flushed earlier, but safe)
             buffer_cache_sync(); // Sync data AND the updated directory entry buffer
        }
        fctx->dirty = false; // Clear flag after attempt
    }

cleanup:
    // Free context and vnode
    kfree(file->vnode->data, sizeof(fat_file_context_t)); // Free the fat_file_context_t
    kfree(file->vnode, sizeof(vnode_t));                 // Free the vnode_t
    file->vnode = NULL; // Important for vfs_close to know it's freed

    return FS_SUCCESS;
}

// fat_readdir remains unimplemented
int fat_readdir(fat_fs_t *fs, const char *p, fat_dir_entry_t **e, size_t *c) {
    (void)fs; (void)p; (void)e; (void)c; // Suppress unused parameter warnings
    terminal_write("[FAT] fat_readdir: Not implemented.\n");
    return -FS_ERR_NOT_SUPPORTED;
}