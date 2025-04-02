#include "fat.h"
#include "terminal.h"
#include "kmalloc.h"
#include "buddy.h"
#include "disk.h"           // Provides disk_read_sectors and disk_write_sectors
#include "vfs.h"            // For VFS integration and driver registration
#include "fs_errno.h"       // For filesystem error codes
#include "fs_init.h"
#include "mount.h"
#include "fat_utils.h"      // Utility functions such as format_filename (definition expected here)
#include "types.h"
#include <string.h>         // For memcpy and memcmp

/* Note: block_read/block_write are replaced by disk_read_sectors/disk_write_sectors */

/* External VFS registration functions */
extern int vfs_register_driver(vfs_driver_t *driver);
extern int vfs_unregister_driver(vfs_driver_t *driver);

/* Forward declarations for functions adapted to VFS signature */
static void *fat_mount_internal(const char *device);
static int fat_unmount_internal(void *fs_context);
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
static int fat_read_internal(file_t *file, void *buf, size_t len);
static int fat_write_internal(file_t *file, const void *buf, size_t len);
static int fat_close_internal(file_t *file);
// static off_t fat_lseek_internal(file_t *file, off_t offset, int whence); // If providing custom lseek

/* ---------------------------- */
/* FAT Table Management Helpers */
/* ---------------------------- */

/* load_fat_table: Reads the FAT table from disk into memory. */
static int load_fat_table(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    size_t table_size = fat_sector_count * bps;
    fs->fat_table = kmalloc(table_size);
    if (!fs->fat_table) {
        terminal_write("[FAT] load_fat_table: Out of memory.\n");
        return -1;
    }
    uint32_t fat_start = fs->boot_sector.reserved_sector_count;
    // Use disk_read_sectors from disk.h
    if (disk_read_sectors(&fs->disk, fat_start, fs->fat_table, fat_sector_count) != 0) {
        terminal_write("[FAT] load_fat_table: Failed to read FAT table from disk.\n");
        kfree(fs->fat_table, table_size);
        fs->fat_table = NULL;
        return -1;
    }
    return 0;
}

/* flush_fat_table: Writes the in‑memory FAT table back to disk. */
static int flush_fat_table(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    uint32_t fat_start = fs->boot_sector.reserved_sector_count;
    // Use disk_write_sectors from disk.h
    if (disk_write_sectors(&fs->disk, fat_start, fs->fat_table, fat_sector_count) != 0) {
        terminal_write("[FAT] flush_fat_table: Failed to write FAT table to disk.\n");
        return -1;
    }
    return 0;
}

/* find_free_cluster: Scans the FAT table for a free cluster (value 0). */
static uint32_t find_free_cluster(fat_fs_t *fs) {
    if (!fs->fat_table) { // Ensure FAT table is loaded
         terminal_write("[FAT] find_free_cluster: FAT table not loaded.\n");
         return 0;
    }
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t total_entries = (fs->fat_size * bps) / sizeof(uint32_t); // Assuming FAT32 entries
    uint32_t *fat = (uint32_t *)fs->fat_table;
    for (uint32_t i = 2; i < total_entries; i++) { // Clusters start at 2
        if ((fat[i] & 0x0FFFFFFF) == 0) { // Free cluster for FAT32
            return i;
        }
    }
    return 0; // No free cluster found
}

/* update_fat_entry: Updates the FAT table entry for a given cluster. (Currently unused) */
static void update_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs->fat_table) return;
    // Assuming FAT32
    uint32_t *fat = (uint32_t *)fs->fat_table;
    // Preserve upper 4 bits, update lower 28
    fat[cluster] = (fat[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
}


/* ---------------------------- */
/* VFS Integration: FAT Driver  */
/* ---------------------------- */

static vfs_driver_t fat_vfs_driver = {
    .fs_name = "FAT32",
    .mount = fat_mount_internal,         // Matches void *(*)(const char *)
    .unmount = fat_unmount_internal,     // Matches int (*)(void *)
    .open = fat_open_internal,           // Matches vnode_t *(*)(void *, const char *, int)
    .read = fat_read_internal,           // Matches int (*)(file_t *, void *, size_t)
    .write = fat_write_internal,         // Matches int (*)(file_t *, const void *, size_t)
    .close = fat_close_internal,         // Matches int (*)(file_t *)
    .lseek = vfs_lseek,                  // Use generic VFS lseek for now
    .next = NULL
};

/* fat_register_driver: Registers the FAT driver with the VFS. */
int fat_register_driver(void) {
    int ret = vfs_register_driver(&fat_vfs_driver);
    if (ret != 0) {
        terminal_write("[FAT] fat_register_driver: Registration failed.\n");
    } else {
        terminal_write("[FAT] FAT driver registered.\n");
    }
    return ret;
}

/* fat_unregister_driver: Unregisters the FAT driver from the VFS. */
void fat_unregister_driver(void) {
    int ret = vfs_unregister_driver(&fat_vfs_driver);
    if (ret != 0) {
        terminal_write("[FAT] fat_unregister_driver: Unregistration failed.\n");
    } else {
        terminal_write("[FAT] FAT driver unregistered.\n");
    }
}

/* -------------------------------- */
/* FAT Implementation Functions     */
/* (matching VFS driver signatures) */
/* -------------------------------- */

/* fat_mount_internal: Implementation for the VFS mount operation. */
static void *fat_mount_internal(const char *device) {
    if (!device) {
        terminal_write("[FAT] Mount: Invalid device parameter.\n");
        return NULL; // Return NULL on failure as expected by VFS
    }

    // Allocate filesystem context structure
    fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t));
    if (!fs) {
        terminal_write("[FAT] Mount: Out of memory for fat_fs_t.\n");
        return NULL;
    }
    memset(fs, 0, sizeof(fat_fs_t)); // Zero initialize

    // Initialize the underlying disk_t structure
    if (disk_init(&fs->disk, device) != 0) {
         terminal_write("[FAT] Mount: Failed to initialize disk.\n");
         kfree(fs, sizeof(fat_fs_t));
         return NULL;
    }
    // fs->device = device; // disk_init already stores this in disk_t

    // Read Boot Sector using disk_read_sectors
    uint8_t *buffer = (uint8_t *)kmalloc(fs->disk.sector_size);
    if (!buffer) {
        terminal_write("[FAT] Mount: Out of memory for boot sector buffer.\n");
        kfree(fs, sizeof(fat_fs_t)); // Cleanup fs allocation
        return NULL;
    }
    if (disk_read_sectors(&fs->disk, 0, buffer, 1) != 0) {
        terminal_write("[FAT] Mount: Failed to read boot sector.\n");
        kfree(buffer, fs->disk.sector_size);
        kfree(fs, sizeof(fat_fs_t));
        return NULL;
    }
    memcpy(&fs->boot_sector, buffer, sizeof(fat_boot_sector_t));
    kfree(buffer, fs->disk.sector_size);

    // Verify boot sector signature
    uint16_t signature = *((uint16_t *)((uint8_t *)&fs->boot_sector + 510));
    if (signature != 0xAA55) {
        terminal_write("[FAT] Mount: Invalid boot sector signature.\n");
        kfree(fs, sizeof(fat_fs_t));
        return NULL;
    }

    // Calculate FAT parameters
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
    fs->root_dir_sectors = root_dir_sectors; // Only relevant for FAT12/16 root dir

    fs->first_data_sector = fs->boot_sector.reserved_sector_count +
                            (fs->boot_sector.num_fats * fat_size) +
                            root_dir_sectors;

    uint32_t data_sectors = total_sectors - fs->first_data_sector;
    if (fs->boot_sector.sectors_per_cluster == 0) {
         terminal_write("[FAT] Mount: Invalid sectors_per_cluster (0).\n");
         kfree(fs, sizeof(fat_fs_t));
         return NULL;
    }
    fs->cluster_count = data_sectors / fs->boot_sector.sectors_per_cluster;

    // Determine FAT type
    if (fs->cluster_count < 4085) {
        fs->type = FAT_TYPE_FAT12;
        terminal_write("[FAT] Mount: FAT12 not fully supported.\n");
         // Allow mounting but warn
    } else if (fs->cluster_count < 65525) {
        fs->type = FAT_TYPE_FAT16;
        terminal_write("[FAT] Mount: FAT16 detected.\n");
        // Adjust logic if needed for FAT16 specific differences (e.g., root dir size)
    } else {
        fs->type = FAT_TYPE_FAT32;
        fs->root_dir_sectors = 0; // FAT32 root dir is in clusters, not fixed size
        terminal_write("[FAT] Mount: FAT32 detected.\n");
    }

    // Load FAT table
    if (load_fat_table(fs) != 0) {
        terminal_write("[FAT] Mount: Failed to load FAT table.\n");
        kfree(fs, sizeof(fat_fs_t));
        return NULL;
    }

    terminal_write("[FAT] Mounted device: ");
    terminal_write(fs->disk.device_name); // Use name from disk_t
    terminal_write(" | Type: ");
    switch(fs->type) {
        case FAT_TYPE_FAT12: terminal_write("FAT12"); break;
        case FAT_TYPE_FAT16: terminal_write("FAT16"); break;
        case FAT_TYPE_FAT32: terminal_write("FAT32"); break;
        default: terminal_write("Unknown"); break;
    }
    terminal_write("\n");

    return fs; // Return the context pointer (fat_fs_t *) cast to void*
}

/* fat_unmount_internal: Implementation for the VFS unmount operation. */
static int fat_unmount_internal(void *fs_context) {
    fat_fs_t *fs = (fat_fs_t *)fs_context; // Cast back from void*
    if (!fs) {
        terminal_write("[FAT] Unmount: Invalid filesystem context.\n");
        return -1;
    }
    // Flush FAT table back to disk
    if (fs->fat_table) {
        if (flush_fat_table(fs) != 0) {
            terminal_write("[FAT] Unmount: Failed to flush FAT table.\n");
            // Continue cleanup even if flush fails? Or return error? For now, continue.
        }
        kfree(fs->fat_table, fs->fat_size * fs->boot_sector.bytes_per_sector);
        fs->fat_table = NULL;
    }
    // Free the context structure itself
    kfree(fs, sizeof(fat_fs_t));
    terminal_write("[FAT] Filesystem unmounted.\n");
    return 0;
}


/* Helper structure to combine VFS file and FAT specific file info */
typedef struct {
    fat_fs_t *fs;            // Associated filesystem
    uint32_t first_cluster;  // First cluster of the file
    uint32_t current_cluster;// Current cluster in file chain
    uint32_t file_size;      // Size of the file in bytes
    uint32_t current_cluster_offset; // Byte offset within the current cluster
    // Add other needed state, e.g., directory entry location for writes
} fat_file_context_t;

/* fat_open_internal: Opens a file and returns a VFS vnode. */
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags) {
    fat_fs_t *fs = (fat_fs_t *)fs_context;
    if (!fs || !path) {
        terminal_write("[FAT] fat_open: Invalid parameters.\n");
        return NULL;
    }

    // --- Find the file in the directory ---
    // This simplified version only looks in the root directory.
    // A full implementation needs to traverse directories based on the path.
    // For FAT32, root starts at fs->boot_sector.root_cluster.

    char fat_filename[11];
    format_filename(path, fat_filename); // Assumes format_filename exists in fat_utils.c/h

    uint32_t dir_cluster;
    uint32_t dir_sector_count;

    // Determine where to start searching (root directory)
    if (fs->type == FAT_TYPE_FAT32) {
        dir_cluster = fs->boot_sector.root_cluster;
        // Read clusters until end or file found (complex for FAT32 root dir spanning clusters)
        // This part needs significant expansion for full FAT32 support.
        terminal_write("[FAT] fat_open: FAT32 directory traversal not fully implemented.\n");
        return NULL; // Simplified: FAT32 root traversal not done here.
    } else { // FAT12/16
        dir_cluster = 0; // Indicates fixed root directory area
        uint32_t root_dir_start_sector = fs->boot_sector.reserved_sector_count +
                                         (fs->boot_sector.num_fats * fs->fat_size);
        dir_sector_count = fs->root_dir_sectors;
        if(dir_sector_count == 0) {
            terminal_write("[FAT] fat_open: Invalid root directory size for FAT12/16.\n");
            return NULL;
        }
         size_t buf_size = dir_sector_count * fs->boot_sector.bytes_per_sector;
         uint8_t *dir_buf = (uint8_t *)kmalloc(buf_size);
         if (!dir_buf) {
             terminal_write("[FAT] fat_open: Out of memory for directory buffer.\n");
             return NULL;
         }

         // Read the entire root directory area
         if (disk_read_sectors(&fs->disk, root_dir_start_sector, dir_buf, dir_sector_count) != 0) {
             terminal_write("[FAT] fat_open: Failed to read root directory sectors.\n");
             kfree(dir_buf, buf_size);
             return NULL;
         }

         // Search for the file entry
         int found = 0;
         fat_dir_entry_t *entry = NULL;
         fat_dir_entry_t *entries = (fat_dir_entry_t *)dir_buf;
         uint32_t root_dir_entries_count = fs->boot_sector.root_entry_count;

         for (uint32_t i = 0; i < root_dir_entries_count; i++) {
             // Use explicit cast to potentially silence warning
             if (entries[i].name[0] == (uint8_t)0x00) break; // End of directory
             if (entries[i].name[0] == (uint8_t)0xE5) continue; // Deleted entry
             // Skip volume label and LFN entries for simplicity
             if ((entries[i].attr & 0x08) || (entries[i].attr & 0x0F) == 0x0F) continue;

             if (memcmp(entries[i].name, fat_filename, 11) == 0) {
                 found = 1;
                 entry = &entries[i];
                 break;
             }
         }

         if (!found) {
             terminal_write("[FAT] fat_open: File not found: ");
             terminal_write(path);
             terminal_write("\n");
             kfree(dir_buf, buf_size);
             return NULL;
         }

         // --- File found, create VFS structures ---
         vnode_t *vnode = (vnode_t *)kmalloc(sizeof(vnode_t));
         fat_file_context_t *file_ctx = (fat_file_context_t *)kmalloc(sizeof(fat_file_context_t));
         if (!vnode || !file_ctx) {
             terminal_write("[FAT] fat_open: Out of memory for vnode/file context.\n");
             if (vnode) kfree(vnode, sizeof(vnode_t));
             if (file_ctx) kfree(file_ctx, sizeof(fat_file_context_t));
             kfree(dir_buf, buf_size);
             return NULL;
         }

         // Populate FAT file context
         file_ctx->fs = fs;
         file_ctx->first_cluster = (((uint32_t)entry->first_cluster_high) << 16) | entry->first_cluster_low;
         file_ctx->current_cluster = file_ctx->first_cluster;
         file_ctx->file_size = entry->file_size;
         file_ctx->current_cluster_offset = 0; // Start at beginning of first cluster

         // Populate VFS vnode
         vnode->data = file_ctx; // Store our FAT context here
         vnode->fs_driver = &fat_vfs_driver; // Link back to our driver

         kfree(dir_buf, buf_size); // Free the directory buffer
         return vnode; // Return the VFS node
    }
    // Add FAT32 root directory handling here if needed
     terminal_write("[FAT] fat_open: Reached end without finding file (or FAT32 root not handled).\n");
    return NULL; // Should not be reached for FAT12/16 if found
}


/* fat_read_internal: Reads data from an open file (VFS context). */
static int fat_read_internal(file_t *file, void *buf, size_t len) {
    if (!file || !file->vnode || !file->vnode->data || !buf) {
        terminal_write("[FAT] fat_read: Invalid parameters.\n");
        return -FS_ERR_INVALID_PARAM; // Use error codes from fs_errno.h
    }

    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    fat_fs_t *fs = fctx->fs;
    size_t total_read = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;

    // Clamp read length to remaining file size
    if (file->offset >= fctx->file_size) {
        return 0; // EOF
    }
    if (file->offset + len > fctx->file_size) {
        len = fctx->file_size - file->offset;
    }

    while (total_read < len) {
        if (fctx->current_cluster < 2 || fctx->current_cluster >= FAT32_EOC) { // Check for valid cluster/EOC
             terminal_write("[FAT] fat_read: Invalid or EOC cluster reached unexpectedly.\n");
             break; // Stop reading
        }

        // Calculate where in the current cluster the read should start
        size_t cluster_read_offset = file->offset % cluster_size;
        size_t bytes_to_read_from_cluster = cluster_size - cluster_read_offset;
        size_t remaining_total_read = len - total_read;

        if (bytes_to_read_from_cluster > remaining_total_read) {
            bytes_to_read_from_cluster = remaining_total_read;
        }

        // Read the cluster (or part of it)
        uint32_t lba = fat_cluster_to_lba(fs, fctx->current_cluster);
        uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) {
            terminal_write("[FAT] fat_read: Out of memory for cluster buffer.\n");
            return -FS_ERR_OUT_OF_MEMORY;
        }

        // Read the necessary sectors for this cluster
        if (disk_read_sectors(&fs->disk, lba, cluster_buf, fs->boot_sector.sectors_per_cluster) != 0) {
            terminal_write("[FAT] fat_read: Failed to read cluster sectors.\n");
            kfree(cluster_buf, cluster_size);
            return -FS_ERR_IO;
        }

        // Copy the data
        memcpy((uint8_t *)buf + total_read, cluster_buf + cluster_read_offset, bytes_to_read_from_cluster);
        kfree(cluster_buf, cluster_size);

        total_read += bytes_to_read_from_cluster;
        file->offset += bytes_to_read_from_cluster; // Update VFS file offset

        // If we read to the end of the cluster, find the next one
        if ((file->offset % cluster_size) == 0 && total_read < len) {
            uint32_t next_cluster = 0;
            if (fat_get_next_cluster(fs, fctx->current_cluster, &next_cluster) != 0) {
                 terminal_write("[FAT] fat_read: Failed to get next cluster from FAT.\n");
                 return -FS_ERR_IO; // Or FS_ERR_CORRUPT
            }
             fctx->current_cluster = next_cluster; // Update context's current cluster
            if (next_cluster >= FAT32_EOC) { // Reached end of chain
                 break;
            }
        }
    }

    return total_read; // Return bytes read
}


/* fat_write_internal: Writes data to an open file (VFS context). */
static int fat_write_internal(file_t *file, const void *buf, size_t len) {
     if (!file || !file->vnode || !file->vnode->data || !buf) {
        terminal_write("[FAT] fat_write: Invalid parameters.\n");
        return -FS_ERR_INVALID_PARAM;
    }

    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    fat_fs_t *fs = fctx->fs;
    size_t total_written = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;

    // Ensure FAT table is loaded (might be redundant if always loaded at mount)
    if (!fs->fat_table) {
        if (load_fat_table(fs) != 0) {
            terminal_write("[FAT] fat_write: Failed to load FAT table.\n");
            return -FS_ERR_IO;
        }
    }

    while (total_written < len) {
        // Allocate cluster if needed (first write or end of chain)
        if (fctx->current_cluster < 2) { // No cluster allocated yet
            uint32_t free_cluster = find_free_cluster(fs);
            if (free_cluster == 0) {
                terminal_write("[FAT] fat_write: No free cluster available.\n");
                return -FS_ERR_NO_SPACE; // Return written count so far? Or error?
            }
            fctx->first_cluster = free_cluster; // Update first cluster if this is the first allocation
            fctx->current_cluster = free_cluster;
             fat_set_cluster_entry(fs, fctx->current_cluster, FAT32_EOC); // Mark as end for now
             // TODO: Update directory entry with first cluster
             flush_fat_table(fs); // Flush change
        }

        // Calculate where in the current cluster the write should start/end
        size_t cluster_write_offset = file->offset % cluster_size;
        size_t bytes_to_write_to_cluster = cluster_size - cluster_write_offset;
        size_t remaining_total_write = len - total_written;

        if (bytes_to_write_to_cluster > remaining_total_write) {
            bytes_to_write_to_cluster = remaining_total_write;
        }

        // Read-Modify-Write the cluster
        uint32_t lba = fat_cluster_to_lba(fs, fctx->current_cluster);
        uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
         if (!cluster_buf) {
            terminal_write("[FAT] fat_write: Out of memory for cluster buffer.\n");
            return -FS_ERR_OUT_OF_MEMORY;
        }

        // Read only if partially overwriting the cluster, otherwise just write
        if (cluster_write_offset != 0 || bytes_to_write_to_cluster < cluster_size) {
             if (disk_read_sectors(&fs->disk, lba, cluster_buf, fs->boot_sector.sectors_per_cluster) != 0) {
                 terminal_write("[FAT] fat_write: Failed to read cluster for modify.\n");
                 kfree(cluster_buf, cluster_size);
                 return -FS_ERR_IO;
             }
        }

        // Copy data from user buffer
        memcpy(cluster_buf + cluster_write_offset, (const uint8_t *)buf + total_written, bytes_to_write_to_cluster);

        // Write the modified cluster back
         if (disk_write_sectors(&fs->disk, lba, cluster_buf, fs->boot_sector.sectors_per_cluster) != 0) {
            terminal_write("[FAT] fat_write: Failed to write cluster.\n");
            kfree(cluster_buf, cluster_size);
            return -FS_ERR_IO;
        }
        kfree(cluster_buf, cluster_size);

        total_written += bytes_to_write_to_cluster;
        file->offset += bytes_to_write_to_cluster;

        // Update file size in context if we wrote past the end
        if (file->offset > fctx->file_size) {
            fctx->file_size = file->offset;
            // TODO: Need to update the directory entry size as well
        }

        // If we filled the cluster and need to write more, get/allocate the next cluster
        if ((file->offset % cluster_size) == 0 && total_written < len) {
             uint32_t next_cluster = 0;
             fat_get_next_cluster(fs, fctx->current_cluster, &next_cluster);

             if (next_cluster < FAT32_EOC) { // Already linked? (Shouldn't happen if we only append?)
                  fctx->current_cluster = next_cluster;
             } else { // End of chain, need new cluster
                  uint32_t free_cluster = find_free_cluster(fs);
                  if (free_cluster == 0) {
                      terminal_write("[FAT] fat_write: No free cluster available while extending file.\n");
                      // Flush FAT before returning partial write count?
                      flush_fat_table(fs);
                      return total_written > 0 ? total_written : -FS_ERR_NO_SPACE;
                  }
                   fat_set_cluster_entry(fs, fctx->current_cluster, free_cluster); // Link old to new
                   fat_set_cluster_entry(fs, free_cluster, FAT32_EOC);       // Mark new as end
                   fctx->current_cluster = free_cluster;                         // Move to new cluster
                   flush_fat_table(fs); // Flush changes
             }
        }
    }
    // TODO: Final flush of FAT? Update directory entry?
    return total_written; // Return bytes written
}

/* fat_close_internal: Closes the file (VFS context). */
static int fat_close_internal(file_t *file) {
    if (!file || !file->vnode || !file->vnode->data) {
        return -FS_ERR_INVALID_PARAM;
    }
    // Free the FAT-specific context stored in vnode->data
    kfree(file->vnode->data, sizeof(fat_file_context_t));
    file->vnode->data = NULL;
    // Free the vnode itself (VFS might do this, or driver should)
    kfree(file->vnode, sizeof(vnode_t));
    file->vnode = NULL;
    // The file_t structure itself is freed by the VFS caller (vfs_close)
    return 0; // Success
}


/* fat_readdir: Reads the root directory and returns an array of directory entries. */
// This function remains largely the same conceptually but needs adjustment
// for FAT32 root directory handling and potentially non-root paths.
// It's not directly part of the VFS driver API shown but is useful.
// Returning -1 for now as it needs significant rework for FAT32/subdirs.
int fat_readdir(fat_fs_t *fs, const char *path, fat_dir_entry_t **entries, size_t *entry_count) {
     terminal_write("[FAT] fat_readdir: Not fully implemented for FAT32/subdirs.\n");
     return -FS_ERR_NOT_SUPPORTED; // Mark as not supported/implemented
}