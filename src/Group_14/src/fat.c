#include "fat.h"
#include "terminal.h"
#include "kmalloc.h"
#include "buddy.h"
#include "block_device.h"
#include "vfs.h"
#include "fs_errno.h"
#include "fs_init.h"
#include "mount.h"
#include "disk.h"
#include "fat_utils.h"¨
#include "types.h"
#include <string.h> // For memcpy, memcmp

/* External block device I/O functions (provided by your disk driver) */
extern int block_read(const char *device, uint32_t lba, void *buffer, size_t count);
extern int block_write(const char *device, uint32_t lba, const void *buffer, size_t count);

/* Forward declaration for VFS registration */
extern int vfs_register_driver(vfs_driver_t *driver);
extern int vfs_unregister_driver(vfs_driver_t *driver);

/* ---------------------------- */
/* FAT Table Management Helpers */
/* ---------------------------- */

/*
 * load_fat_table
 *
 * Reads the FAT table from disk into memory.
 * For FAT32, each entry is 32 bits.
 */
static int load_fat_table(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    size_t table_size = fat_sector_count * bps;
    fs->fat_table = kmalloc(table_size);
    if (!fs->fat_table) {
        terminal_write("[FAT] load_fat_table: Out of memory.\n");
        return -1;
    }
    // The FAT table starts at the first FAT sector (reserved_sector_count)
    uint32_t fat_start = fs->boot_sector.reserved_sector_count;
    if (block_read(fs->device, fat_start, fs->fat_table, fat_sector_count) != 0) {
        terminal_write("[FAT] load_fat_table: Failed to read FAT table from disk.\n");
        kfree(fs->fat_table, table_size);
        fs->fat_table = NULL;
        return -1;
    }
    return 0;
}

/*
 * flush_fat_table
 *
 * Writes the in‑memory FAT table back to disk.
 */
static int flush_fat_table(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    uint32_t fat_start = fs->boot_sector.reserved_sector_count;
    if (block_write(fs->device, fat_start, fs->fat_table, fat_sector_count) != 0) {
        terminal_write("[FAT] flush_fat_table: Failed to write FAT table to disk.\n");
        return -1;
    }
    return 0;
}

/*
 * find_free_cluster
 *
 * Scans the in‑memory FAT table for a free cluster (value 0).
 * Returns the cluster number if found, or 0 if none available.
 */
static uint32_t find_free_cluster(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t total_entries = (fs->fat_size * bps) / sizeof(uint32_t);
    uint32_t *fat = (uint32_t *)fs->fat_table;
    for (uint32_t i = 2; i < total_entries; i++) { // Clusters start at 2
        if ((fat[i] & 0x0FFFFFFF) == 0) { // Free cluster
            return i;
        }
    }
    return 0;
}

/*
 * update_fat_entry
 *
 * Updates the FAT table entry for a given cluster.
 */
static void update_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t *fat = (uint32_t *)fs->fat_table;
    fat[cluster] = value;
}

/* ---------------------------- */
/* VFS Integration: FAT Driver  */
/* ---------------------------- */

/*
 * Define a static vfs_driver_t structure for the FAT driver.
 * (Assumes vfs_driver_t is defined in your VFS layer.)
 */
static vfs_driver_t fat_vfs_driver = {
    .fs_name = "FAT32",  // We assume FAT32 for our write implementation.
    .mount = (void *(*)(const char *))fat_mount,
    .unmount = (int (*)(void *))fat_unmount,
    .open = (vnode_t *(*)(void *, const char *, int))fat_open,
    .read = (int (*)(file_t *, void *, size_t))fat_read,
    .write = (int (*)(file_t *, const void *, size_t))fat_write,
    .close = (int (*)(file_t *))fat_close,
    .lseek = (off_t (*)(file_t *, off_t, int))vfs_lseek, // Assume VFS provides a default lseek or use our implementation below.
    .next = NULL
};

/*
 * fat_register_driver
 *
 * Registers the FAT driver with the VFS.
 */
int fat_register_driver(void) {
    int ret = vfs_register_driver(&fat_vfs_driver);
    if (ret != 0) {
        terminal_write("[FAT] fat_register_driver: Registration failed.\n");
    } else {
        terminal_write("[FAT] FAT driver registered.\n");
    }
    return ret;
}

/*
 * fat_unregister_driver
 *
 * Unregisters the FAT driver from the VFS.
 */
void fat_unregister_driver(void) {
    int ret = vfs_unregister_driver(&fat_vfs_driver);
    if (ret != 0) {
        terminal_write("[FAT] fat_unregister_driver: Unregistration failed.\n");
    } else {
        terminal_write("[FAT] FAT driver unregistered.\n");
    }
}

/* ---------------------------- */
/* FAT Filesystem API Functions */
/* ---------------------------- */

/*
 * fat_mount
 * Reads the boot sector, verifies the signature, calculates parameters,
 * determines FAT type, loads the FAT table, and returns success.
 */
int fat_mount(const char *device, fat_fs_t *fs) {
    if (!device || !fs) {
        terminal_write("[FAT] Mount: Invalid parameters.\n");
        return -1;
    }
    fs->device = device;

    uint8_t *buffer = (uint8_t *)kmalloc(512);
    if (!buffer) {
        terminal_write("[FAT] Mount: Out of memory for boot sector buffer.\n");
        return -1;
    }
    if (block_read(device, 0, buffer, 1) != 0) {
        terminal_write("[FAT] Mount: Failed to read boot sector.\n");
        kfree(buffer, 512);
        return -1;
    }
    memcpy(&fs->boot_sector, buffer, sizeof(fat_boot_sector_t));
    kfree(buffer, 512);

    // Verify boot sector signature (0xAA55 at offset 510)
    uint16_t signature = *((uint16_t *)((uint8_t *)&fs->boot_sector + 510));
    if (signature != 0xAA55) {
        terminal_write("[FAT] Mount: Invalid boot sector signature.\n");
        return -1;
    }

    uint32_t total_sectors = (fs->boot_sector.total_sectors_short != 0) ?
                                fs->boot_sector.total_sectors_short :
                                fs->boot_sector.total_sectors_long;
    uint32_t fat_size = (fs->boot_sector.fat_size_16 != 0) ?
                           fs->boot_sector.fat_size_16 :
                           fs->boot_sector.fat_size_32;
    fs->total_sectors = total_sectors;
    fs->fat_size = fat_size;

    uint32_t root_dir_sectors = ((fs->boot_sector.root_entry_count * 32) +
                                 (fs->boot_sector.bytes_per_sector - 1)) / fs->boot_sector.bytes_per_sector;
    fs->root_dir_sectors = root_dir_sectors;

    uint32_t first_data_sector = fs->boot_sector.reserved_sector_count +
                                 (fs->boot_sector.num_fats * fat_size) +
                                 root_dir_sectors;
    fs->first_data_sector = first_data_sector;

    uint32_t data_sectors = total_sectors - first_data_sector;
    fs->cluster_count = data_sectors / fs->boot_sector.sectors_per_cluster;

    if (fs->cluster_count < 4085) {
        fs->type = FAT_TYPE_FAT12;
    } else if (fs->cluster_count < 65525) {
        fs->type = FAT_TYPE_FAT16;
    } else {
        fs->type = FAT_TYPE_FAT32;
    }

    if (load_fat_table(fs) != 0) {
        terminal_write("[FAT] Mount: Failed to load FAT table.\n");
        return -1;
    }

    terminal_write("[FAT] Mounted device: ");
    terminal_write(device);
    terminal_write(" | Type: ");
    switch(fs->type) {
        case FAT_TYPE_FAT12: terminal_write("FAT12"); break;
        case FAT_TYPE_FAT16: terminal_write("FAT16"); break;
        case FAT_TYPE_FAT32: terminal_write("FAT32"); break;
        default: terminal_write("Unknown"); break;
    }
    terminal_write("\n");
    return 0;
}

/*
 * fat_unmount
 * Unmounts the filesystem by flushing the FAT table and freeing resources.
 */
int fat_unmount(fat_fs_t *fs) {
    if (!fs) {
        terminal_write("[FAT] Unmount: Invalid filesystem pointer.\n");
        return -1;
    }
    // Flush FAT table back to disk (if modified)
    if (fs->fat_table) {
        if (flush_fat_table(fs) != 0) {
            terminal_write("[FAT] Unmount: Failed to flush FAT table.\n");
            return -1;
        }
        kfree(fs->fat_table, fs->fat_size * fs->boot_sector.bytes_per_sector);
        fs->fat_table = NULL;
    }
    terminal_write("[FAT] Filesystem unmounted.\n");
    return 0;
}

/*
 * fat_open
 * Opens a file in the root directory. This simplified implementation
 * assumes files reside in the root.
 */
int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file) {
    if (!fs || !path || !file) {
        terminal_write("[FAT] fat_open: Invalid parameters.\n");
        return -1;
    }
    char fat_filename[11];
    // Use the helper to format the filename into FAT 8.3 format.
    format_filename(path, fat_filename);

    uint32_t root_dir_sector = fs->boot_sector.reserved_sector_count +
                               (fs->boot_sector.num_fats * ((fs->boot_sector.fat_size_16) ? fs->boot_sector.fat_size_16 : fs->boot_sector.fat_size_32));
    uint32_t root_dir_entries = fs->boot_sector.root_entry_count;
    uint32_t root_dir_size = ((root_dir_entries * 32) + (fs->boot_sector.bytes_per_sector - 1)) / fs->boot_sector.bytes_per_sector;
    size_t buf_size = root_dir_size * fs->boot_sector.bytes_per_sector;

    uint8_t *dir_buf = (uint8_t *)kmalloc(buf_size);
    if (!dir_buf) {
        terminal_write("[FAT] fat_open: Out of memory for directory buffer.\n");
        return -1;
    }
    for (uint32_t i = 0; i < root_dir_size; i++) {
        if (block_read(fs->device, root_dir_sector + i, dir_buf + (i * fs->boot_sector.bytes_per_sector), 1) != 0) {
            terminal_write("[FAT] fat_open: Failed to read root directory sector.\n");
            kfree(dir_buf, buf_size);
            return -1;
        }
    }
    int found = 0;
    fat_dir_entry_t *entries = (fat_dir_entry_t *)dir_buf;
    for (uint32_t i = 0; i < root_dir_entries; i++) {
        if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5)
            continue;
        if (memcmp(entries[i].name, fat_filename, 11) == 0) {
            found = 1;
            file->fs = fs;
            file->first_cluster = (((uint32_t)entries[i].first_cluster_high) << 16) | entries[i].first_cluster_low;
            file->current_cluster = file->first_cluster;
            file->file_size = entries[i].file_size;
            file->pos = 0;
            break;
        }
    }
    kfree(dir_buf, buf_size);
    if (!found) {
        terminal_write("[FAT] fat_open: File not found: ");
        terminal_write(path);
        terminal_write("\n");
        return -1;
    }
    return 0;
}

/*
 * fat_read
 * Reads up to 'len' bytes from an open FAT file into 'buf'.
 * This function follows the cluster chain. (For simplicity, only direct clusters are handled.)
 */
int fat_read(fat_fs_t *fs, fat_file_t *file, void *buf, size_t len, size_t *read_bytes) {
    if (!fs || !file || !buf) {
        terminal_write("[FAT] fat_read: Invalid parameters.\n");
        return -1;
    }
    size_t total_read = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    uint32_t current_cluster = file->current_cluster;

    while (total_read < len && file->pos < file->file_size && current_cluster >= 2) {
        uint32_t lba = fs->first_data_sector + ((current_cluster - 2) * fs->boot_sector.sectors_per_cluster);
        uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) {
            terminal_write("[FAT] fat_read: Out of memory for cluster buffer.\n");
            return -1;
        }
        for (uint32_t i = 0; i < fs->boot_sector.sectors_per_cluster; i++) {
            if (block_read(fs->device, lba + i, cluster_buf + (i * fs->boot_sector.bytes_per_sector), 1) != 0) {
                terminal_write("[FAT] fat_read: Failed to read cluster sector.\n");
                kfree(cluster_buf, cluster_size);
                return -1;
            }
        }
        size_t offset = file->pos % cluster_size;
        size_t available = cluster_size - offset;
        size_t to_copy = ((len - total_read) < available) ? (len - total_read) : available;
        if (file->pos + to_copy > file->file_size)
            to_copy = file->file_size - file->pos;
        memcpy((uint8_t *)buf + total_read, cluster_buf + offset, to_copy);
        total_read += to_copy;
        file->pos += to_copy;
        kfree(cluster_buf, cluster_size);
        if (file->pos >= file->file_size)
            break;
        /* In a complete implementation, consult the FAT table to get the next cluster.
         * Here we simulate by incrementing the cluster number.
         */
        current_cluster++;
        file->current_cluster = current_cluster;
    }
    if (read_bytes)
        *read_bytes = total_read;
    return 0;
}

/*
 * fat_write
 * Writes data to a FAT file. This implementation supports extending a file
 * by allocating new clusters. It updates the in‑memory FAT table and flushes
 * changes to disk. (This is a simplified FAT32 implementation.)
 */
int fat_write(fat_fs_t *fs, fat_file_t *file, const void *buf, size_t len, size_t *written_bytes) {
    if (!fs || !file || !buf) {
        terminal_write("[FAT] fat_write: Invalid parameters.\n");
        return -1;
    }
    size_t total_written = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    uint32_t bps = fs->boot_sector.bytes_per_sector;

    // Ensure FAT table is loaded.
    if (!fs->fat_table) {
        if (load_fat_table(fs) != 0) {
            terminal_write("[FAT] fat_write: Failed to load FAT table.\n");
            return -1;
        }
    }

    /* For simplicity, we assume file data is contiguous.
     * Extend file if necessary.
     */
    uint32_t current_cluster = file->current_cluster;
    uint32_t *fat = (uint32_t *)fs->fat_table;
    // If current cluster is marked as end-of-chain, allocate a new cluster.
    if ((fat[current_cluster] & 0x0FFFFFFF) >= FAT32_EOC) {
        uint32_t free_cluster = find_free_cluster(fs);
        if (free_cluster == 0) {
            terminal_write("[FAT] fat_write: No free cluster available.\n");
            return -1;
        }
        // Update FAT table: link current cluster to free_cluster and mark free_cluster as EOC.
        fat[current_cluster] = free_cluster;
        fat[free_cluster] = FAT32_EOC;
        flush_fat_table(fs);
        current_cluster = free_cluster;
        file->current_cluster = current_cluster;
    }

    while (total_written < len) {
        // Calculate offset within current cluster.
        size_t cluster_offset = file->pos % cluster_size;
        size_t available = cluster_size - cluster_offset;
        size_t to_write = (len - total_written < available) ? (len - total_written) : available;

        // Read the current cluster into a buffer.
        uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) {
            terminal_write("[FAT] fat_write: Out of memory for cluster buffer.\n");
            return -1;
        }
        uint32_t lba = fs->first_data_sector + ((current_cluster - 2) * fs->boot_sector.sectors_per_cluster);
        if (block_read(fs->device, lba, cluster_buf, fs->boot_sector.sectors_per_cluster) != 0) {
            terminal_write("[FAT] fat_write: Failed to read current cluster.\n");
            kfree(cluster_buf, cluster_size);
            return -1;
        }

        // Copy data from input buffer into the cluster buffer.
        memcpy(cluster_buf + cluster_offset, (uint8_t *)buf + total_written, to_write);
        // Write the modified cluster back to disk.
        if (block_write(fs->device, lba, cluster_buf, fs->boot_sector.sectors_per_cluster) != 0) {
            terminal_write("[FAT] fat_write: Failed to write current cluster.\n");
            kfree(cluster_buf, cluster_size);
            return -1;
        }
        kfree(cluster_buf, cluster_size);

        file->pos += to_write;
        total_written += to_write;

        // If we have written to the entire current cluster and still have data,
        // allocate a new cluster.
        if (to_write == available && total_written < len) {
            uint32_t free_cluster = find_free_cluster(fs);
            if (free_cluster == 0) {
                terminal_write("[FAT] fat_write: No free cluster available while extending file.\n");
                break;
            }
            fat[current_cluster] = free_cluster;
            fat[free_cluster] = FAT32_EOC;
            flush_fat_table(fs);
            current_cluster = free_cluster;
            file->current_cluster = current_cluster;
        }
    }

    // Update file size if we've extended the file.
    if (file->pos > file->file_size) {
        file->file_size = file->pos;
    }
    if (written_bytes)
        *written_bytes = total_written;
    return total_written;
}

/*
 * fat_close
 * Closes an open FAT file. No dynamic state is maintained, so simply return success.
 */
int fat_close(fat_fs_t *fs, fat_file_t *file) {
    (void)fs;
    (void)file;
    return 0;
}

/*
 * fat_readdir
 * Reads the root directory and returns an array of directory entries.
 */
int fat_readdir(fat_fs_t *fs, const char *path, fat_dir_entry_t **entries, size_t *entry_count) {
    if (!fs || !path || !entries || !entry_count) {
        terminal_write("[FAT] fat_readdir: Invalid parameters.\n");
        return -1;
    }
    uint32_t root_dir_sector = fs->boot_sector.reserved_sector_count +
        (fs->boot_sector.num_fats * ((fs->boot_sector.fat_size_16) ? fs->boot_sector.fat_size_16 : fs->boot_sector.fat_size_32));
    uint32_t root_dir_entries = fs->boot_sector.root_entry_count;
    uint32_t root_dir_size = ((root_dir_entries * 32) + (fs->boot_sector.bytes_per_sector - 1)) / fs->boot_sector.bytes_per_sector;
    size_t buf_size = root_dir_size * fs->boot_sector.bytes_per_sector;
    
    uint8_t *dir_buf = (uint8_t *)kmalloc(buf_size);
    if (!dir_buf) {
        terminal_write("[FAT] fat_readdir: Out of memory for directory buffer.\n");
        return -1;
    }
    for (uint32_t i = 0; i < root_dir_size; i++) {
        if (block_read(fs->device, root_dir_sector + i, dir_buf + (i * fs->boot_sector.bytes_per_sector), 1) != 0) {
            terminal_write("[FAT] fat_readdir: Failed to read root directory sector.\n");
            kfree(dir_buf, buf_size);
            return -1;
        }
    }
    size_t count = 0;
    fat_dir_entry_t *dir_entries = (fat_dir_entry_t *)dir_buf;
    for (uint32_t i = 0; i < root_dir_entries; i++) {
        if (dir_entries[i].name[0] != 0x00 && dir_entries[i].name[0] != 0xE5)
            count++;
    }
    fat_dir_entry_t *entry_array = (fat_dir_entry_t *)kmalloc(count * sizeof(fat_dir_entry_t));
    if (!entry_array) {
        terminal_write("[FAT] fat_readdir: Out of memory for entry array.\n");
        kfree(dir_buf, buf_size);
        return -1;
    }
    size_t j = 0;
    for (uint32_t i = 0; i < root_dir_entries; i++) {
        if (dir_entries[i].name[0] != 0x00 && dir_entries[i].name[0] != 0xE5) {
            memcpy(&entry_array[j], &dir_entries[i], sizeof(fat_dir_entry_t));
            j++;
        }
    }
    *entries = entry_array;
    *entry_count = count;
    kfree(dir_buf, buf_size);
    return 0;
}
