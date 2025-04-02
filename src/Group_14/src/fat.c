#include "fat.h"
#include "terminal.h"
#include "kmalloc.h"
#include "buddy.h"
#include "disk.h"
#include "vfs.h"
#include "fs_errno.h"
// #include "fs_init.h" // Not directly needed
// #include "mount.h" // Not directly needed
#include "fat_utils.h"      // Needs fat_cluster_to_lba, fat_get_next_cluster, fat_set_cluster_entry, format_filename
#include "buffer_cache.h"
#include "sys_file.h"       // Includes O_* flags
#include "types.h"
#include <string.h>         // For memcpy and memcmp

// --- Forward Declarations & Externs ---
extern int vfs_register_driver(vfs_driver_t *driver);
extern int vfs_unregister_driver(vfs_driver_t *driver);
static void *fat_mount_internal(const char *device);
static int fat_unmount_internal(void *fs_context);
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags);
static int fat_read_internal(file_t *file, void *buf, size_t len);
static int fat_write_internal(file_t *file, const void *buf, size_t len);
static int fat_close_internal(file_t *file);
static int read_cluster(fat_fs_t *fs, uint32_t clu, uint32_t off, void *buf, size_t len);
static int write_cluster(fat_fs_t *fs, uint32_t clu, uint32_t off, const void *buf, size_t len);
static int find_directory_entry(fat_fs_t *fs, const char *path, fat_dir_entry_t *entry, uint32_t *e_cl, uint32_t *e_off);
static int load_fat_table(fat_fs_t *fs);
static int flush_fat_table(fat_fs_t *fs);
static uint32_t find_free_cluster(fat_fs_t *fs);
static void fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value); // Replaces update_fat_entry

// --- FAT Table Management Helpers ---

/* Reads a specific sector relative to the start of the FAT */
static int read_fat_sector(fat_fs_t *fs, uint32_t sector_offset, uint8_t *buffer) {
    uint32_t fat_start_lba = fs->boot_sector.reserved_sector_count;
    uint32_t target_lba = fat_start_lba + sector_offset;
    size_t sector_size = fs->boot_sector.bytes_per_sector;
    buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
    if (!buf) { /* error */ return -FS_ERR_IO; }
    memcpy(buffer, buf->data, sector_size);
    buffer_release(buf);
    return FS_SUCCESS;
}

/* Writes a specific sector relative to the start of the FAT */
static int write_fat_sector(fat_fs_t *fs, uint32_t sector_offset, const uint8_t *buffer) {
    uint32_t fat_start_lba = fs->boot_sector.reserved_sector_count;
    uint32_t target_lba = fat_start_lba + sector_offset;
    size_t sector_size = fs->boot_sector.bytes_per_sector;
    buffer_t *buf = buffer_get(fs->disk.blk_dev.device_name, target_lba);
    if (!buf) { /* error */ return -FS_ERR_IO; }
    memcpy(buf->data, buffer, sector_size);
    buffer_mark_dirty(buf);
    buffer_release(buf);
    return FS_SUCCESS;
}

/* load_fat_table: Reads the FAT table from disk into memory using buffer cache. */
static int load_fat_table(fat_fs_t *fs) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    if (fat_sector_count == 0 || bps == 0) return -FS_ERR_INVALID_FORMAT;
    size_t table_size = fat_sector_count * bps;
    fs->fat_table = kmalloc(table_size);
    if (!fs->fat_table) return -FS_ERR_OUT_OF_MEMORY;

    uint8_t *current_ptr = (uint8_t *)fs->fat_table;
    for (uint32_t i = 0; i < fat_sector_count; i++) {
        if (read_fat_sector(fs, i, current_ptr) != FS_SUCCESS) {
            kfree(fs->fat_table, table_size); fs->fat_table = NULL; return -FS_ERR_IO;
        }
        current_ptr += bps;
    }
    terminal_write("[FAT] FAT table loaded.\n");
    return FS_SUCCESS;
}

/* flush_fat_table: Writes the in‑memory FAT table back to disk using buffer cache. */
static int flush_fat_table(fat_fs_t *fs) {
    if (!fs->fat_table) return -FS_ERR_INVALID_PARAM;
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t fat_sector_count = fs->fat_size;
    const uint8_t *current_ptr = (const uint8_t *)fs->fat_table;
    for (uint32_t i = 0; i < fat_sector_count; i++) {
        if (write_fat_sector(fs, i, current_ptr) != FS_SUCCESS) return -FS_ERR_IO;
        current_ptr += bps;
    }
    terminal_write("[FAT] FAT table flush requested.\n");
    return FS_SUCCESS;
}

/* find_free_cluster: Scans the FAT table for a free cluster (value 0). */
static uint32_t find_free_cluster(fat_fs_t *fs) {
    if (!fs->fat_table) return 0;
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    uint32_t total_entries = 0;
    uint32_t *fat32 = NULL; uint16_t *fat16 = NULL;

    if (fs->type == FAT_TYPE_FAT32) {
        if (bps == 0) return 0; total_entries = (fs->fat_size * bps) / sizeof(uint32_t); fat32 = (uint32_t *)fs->fat_table;
    } else if (fs->type == FAT_TYPE_FAT16) {
        if (bps == 0) return 0; total_entries = (fs->fat_size * bps) / sizeof(uint16_t); fat16 = (uint16_t *)fs->fat_table;
    } else { return 0; } // FAT12 unsupported

    for (uint32_t i = 2; i < total_entries; i++) { // Clusters 0/1 reserved
        if (fs->type == FAT_TYPE_FAT32 && (fat32[i] & 0x0FFFFFFF) == 0) return i;
        if (fs->type == FAT_TYPE_FAT16 && fat16[i] == 0) return i;
    }
    terminal_write("[FAT] find_free_cluster: No free cluster found.\n"); return 0;
}

/* Replaces update_fat_entry - needed by write logic */
static void fat_set_cluster_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
     // Assumes fat_table is loaded and valid cluster number
     // Needs implementation from fat_utils.c or here.
     // This is critical for write support.
     terminal_printf("[FAT stub] fat_set_cluster_entry: cluster=%u, value=0x%x\n", cluster, value);
     // Placeholder - Needs actual implementation from fat_utils.c
     if (!fs->fat_table) return;
     if (fs->type == FAT_TYPE_FAT32) {
          uint32_t *fat = (uint32_t*)fs->fat_table;
          if (cluster < (fs->fat_size * fs->boot_sector.bytes_per_sector / 4)) { // Bounds check
             fat[cluster] = (fat[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
          }
     } else if (fs->type == FAT_TYPE_FAT16) {
          uint16_t *fat = (uint16_t*)fs->fat_table;
           if (cluster < (fs->fat_size * fs->boot_sector.bytes_per_sector / 2)) { // Bounds check
             fat[cluster] = (uint16_t)value;
           }
     }
}

// --- VFS Driver Registration ---
static vfs_driver_t fat_vfs_driver = { /* ... same ... */ };
int fat_register_driver(void) { /* ... same ... */ return vfs_register_driver(&fat_vfs_driver); }
void fat_unregister_driver(void) { /* ... same ... */ vfs_unregister_driver(&fat_vfs_driver); }

// --- FAT Implementation Functions ---
static void *fat_mount_internal(const char *device) {
     // Reuse previous working implementation
     fat_fs_t *fs = (fat_fs_t *)kmalloc(sizeof(fat_fs_t)); if (!fs) return NULL; memset(fs, 0, sizeof(fat_fs_t));
     if (disk_init(&fs->disk, device)!=0) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     size_t sector_size = fs->disk.blk_dev.sector_size; if (sector_size == 0) { kfree(fs, sizeof(fat_fs_t)); return NULL;}
     buffer_t *bs_buf = buffer_get(device, 0); if (!bs_buf) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     memcpy(&fs->boot_sector, bs_buf->data, sizeof(fat_boot_sector_t)); buffer_release(bs_buf);
     if (fs->boot_sector.boot_sector_signature != 0xAA55) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     if (fs->boot_sector.bytes_per_sector == 0) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     fs->disk.blk_dev.sector_size = fs->boot_sector.bytes_per_sector; sector_size = fs->disk.blk_dev.sector_size;
     // Calculate parameters...
     uint32_t total_sectors = (fs->boot_sector.total_sectors_short!=0)?fs->boot_sector.total_sectors_short:fs->boot_sector.total_sectors_long;
     uint32_t fat_size = (fs->boot_sector.fat_size_16!=0)?fs->boot_sector.fat_size_16:fs->boot_sector.fat_size_32;
     fs->total_sectors = total_sectors; fs->fat_size = fat_size;
     uint32_t root_dir_sectors = ((fs->boot_sector.root_entry_count * 32) + (sector_size - 1)) / sector_size;
     fs->root_dir_sectors = root_dir_sectors;
     fs->first_data_sector = fs->boot_sector.reserved_sector_count + (fs->boot_sector.num_fats * fat_size) + root_dir_sectors;
     if (fs->boot_sector.sectors_per_cluster == 0) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     uint32_t data_sectors = total_sectors - fs->first_data_sector;
     fs->cluster_count = data_sectors / fs->boot_sector.sectors_per_cluster;
     // Determine type...
     if (fs->cluster_count < 4085) fs->type = FAT_TYPE_FAT12; else if (fs->cluster_count < 65525) fs->type = FAT_TYPE_FAT16; else fs->type = FAT_TYPE_FAT32;
     if (fs->type == FAT_TYPE_FAT32) fs->root_dir_sectors = 0;
     // Update driver name
     if (fs->type == FAT_TYPE_FAT16) fat_vfs_driver.fs_name = "FAT16"; else if (fs->type == FAT_TYPE_FAT12) fat_vfs_driver.fs_name = "FAT12"; else fat_vfs_driver.fs_name = "FAT32";
     // Load FAT table...
     if (load_fat_table(fs) != 0) { kfree(fs, sizeof(fat_fs_t)); return NULL; }
     terminal_printf("[FAT] Mounted: %s (%s)\n", fs->disk.blk_dev.device_name, fat_vfs_driver.fs_name);
     return fs;
}
static int fat_unmount_internal(void *fs_context) {
    // Reuse previous working implementation
    fat_fs_t *fs = (fat_fs_t *)fs_context; if (!fs) return -FS_ERR_INVALID_PARAM;
    if (fs->fat_table) {
        if (flush_fat_table(fs)!=0) { /* Warning */ }
        kfree(fs->fat_table, fs->fat_size * fs->boot_sector.bytes_per_sector); fs->fat_table = NULL;
    } buffer_cache_sync(); kfree(fs, sizeof(fat_fs_t)); terminal_write("[FAT] Unmounted.\n"); return FS_SUCCESS;
}

// --- FAT File Context ---
typedef struct {
    fat_fs_t *fs; uint32_t first_cluster; uint32_t current_cluster; uint32_t file_size;
    // Add fields to store location of directory entry for updates
    uint32_t dir_entry_cluster;     // Cluster containing the entry (0 for FAT16 root)
    uint32_t dir_entry_offset;      // Byte offset within the cluster/root area
    bool dirty;                     // Flag if size/metadata changed
} fat_file_context_t;

// --- Cluster Read/Write Helpers ---
static int read_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, void *buf, size_t len) {
    // Uses buffer cache - Reuse previous implementation
    if (cluster < 2 || !fs || !buf) return -FS_ERR_INVALID_PARAM;
    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    size_t sector_size = fs->boot_sector.bytes_per_sector; size_t sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
    size_t cluster_size = sector_size * sectors_per_cluster; if (cluster_size == 0 || sector_size == 0) return -FS_ERR_INVALID_FORMAT;
    size_t bytes_read = 0; uint8_t *out_buf = (uint8_t *)buf;
    while (bytes_read < len) {
        uint32_t sec_off = (offset_in_cluster + bytes_read) / sector_size; uint32_t off_in_sec = (offset_in_cluster + bytes_read) % sector_size;
        size_t read_len = sector_size - off_in_sec; if (read_len > (len - bytes_read)) read_len = len - bytes_read;
        if (sec_off >= sectors_per_cluster) break;
        buffer_t *b = buffer_get(fs->disk.blk_dev.device_name, lba + sec_off); if (!b) return -FS_ERR_IO;
        memcpy(out_buf + bytes_read, b->data + off_in_sec, read_len); buffer_release(b); bytes_read += read_len;
    } return bytes_read;
}

static int write_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t offset_in_cluster, const void *buf, size_t len) {
    // Uses buffer cache - Reuse previous implementation
    if (cluster < 2 || !fs || !buf) return -FS_ERR_INVALID_PARAM;
    uint32_t lba = fat_cluster_to_lba(fs, cluster);
    size_t sector_size = fs->boot_sector.bytes_per_sector; size_t sectors_per_cluster = fs->boot_sector.sectors_per_cluster;
    size_t cluster_size = sector_size * sectors_per_cluster; if (cluster_size == 0 || sector_size == 0) return -FS_ERR_INVALID_FORMAT;
    size_t bytes_written = 0; const uint8_t *in_buf = (const uint8_t *)buf;
    while (bytes_written < len) {
        uint32_t sec_off = (offset_in_cluster + bytes_written) / sector_size; uint32_t off_in_sec = (offset_in_cluster + bytes_written) % sector_size;
        size_t write_len = sector_size - off_in_sec; if (write_len > (len - bytes_written)) write_len = len - bytes_written;
        if (sec_off >= sectors_per_cluster) break;
        buffer_t *b = buffer_get(fs->disk.blk_dev.device_name, lba + sec_off); if (!b) return -FS_ERR_IO;
        memcpy(b->data + off_in_sec, in_buf + bytes_written, write_len); buffer_mark_dirty(b); buffer_release(b); bytes_written += write_len;
    } return bytes_written;
}

/**
 * find_directory_entry (With FAT32 Root Search)
 */
static int find_directory_entry(fat_fs_t *fs, const char *path, fat_dir_entry_t *entry, uint32_t *entry_cluster_num, uint32_t *entry_offset_in_cluster) {
     // ... (Implementation from previous response - crucial that this works) ...
      // Ensure it handles both FAT16 root area and FAT32 root cluster chain
      // Return FS_SUCCESS on find, FS_ERR_NOT_FOUND, or other FS_ERR_*
       terminal_printf("[FAT DEBUG] find_directory_entry: path='%s'\n", path);
       bool is_subdir_path = (strchr(path, '/') != NULL && !(path[0] == '/' && path[1] == '\0'));
       if (is_subdir_path) { /* ... return error ... */ return -FS_ERR_NOT_SUPPORTED; }
       char fat_filename[11]; const char *basename = strrchr(path, '/'); basename = basename ? basename+1 : path;
       format_filename(basename, fat_filename);
       terminal_printf("  Searching for: '%.11s'\n", fat_filename);

       size_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
       if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
       size_t entries_per_cluster = cluster_size / sizeof(fat_dir_entry_t);

       if (fs->type == FAT_TYPE_FAT16 || fs->type == FAT_TYPE_FAT12) { /* Search fixed root */
           // ... (FAT16/12 root search logic using buffer_get) ...
            return -FS_ERR_NOT_FOUND; // Placeholder
       } else if (fs->type == FAT_TYPE_FAT32) { /* Search root cluster chain */
            uint32_t current_cluster = fs->boot_sector.root_cluster;
            uint8_t* cluster_buffer = (uint8_t*)kmalloc(cluster_size); if (!cluster_buffer) return -FS_ERR_OUT_OF_MEMORY;
            while (current_cluster >= 2 && current_cluster < 0x0FFFFFF8) {
                 int read_res = read_cluster(fs, current_cluster, 0, cluster_buffer, cluster_size); if (read_res<0) {kfree(cluster_buffer, cluster_size); return read_res;}
                 fat_dir_entry_t *entries = (fat_dir_entry_t *)cluster_buffer;
                 for (size_t i = 0; i < entries_per_cluster; ++i) {
                      if (entries[i].name[0] == 0x00) { kfree(cluster_buffer, cluster_size); return -FS_ERR_NOT_FOUND; }
                      if ((uint8_t)entries[i].name[0] == 0xE5) continue;
                      if ((entries[i].attr & 0x08) || (entries[i].attr & 0x0F) == 0x0F) continue;
                      if (memcmp(entries[i].name, fat_filename, 11) == 0) {
                           memcpy(entry, &entries[i], sizeof(fat_dir_entry_t));
                           if (entry_cluster_num) *entry_cluster_num = current_cluster;
                           if (entry_offset_in_cluster) *entry_offset_in_cluster = i * sizeof(fat_dir_entry_t);
                           kfree(cluster_buffer, cluster_size); return FS_SUCCESS;
                      }
                 }
                 uint32_t next_cluster; if (fat_get_next_cluster(fs, current_cluster, &next_cluster)!=0) { kfree(cluster_buffer, cluster_size); return -FS_ERR_IO; }
                 current_cluster = next_cluster;
            } kfree(cluster_buffer, cluster_size); return -FS_ERR_NOT_FOUND;
       } else { return -FS_ERR_NOT_SUPPORTED; }
}

/**
 * fat_open_internal
 */
static vnode_t *fat_open_internal(void *fs_context, const char *path, int flags) {
    fat_fs_t *fs = (fat_fs_t *)fs_context; if (!fs || !path) return NULL;
    terminal_printf("[FAT] fat_open_internal: path='%s', flags=0x%x\n", path, flags);

    fat_dir_entry_t entry; uint32_t dir_cluster, dir_offset;
    int find_res = find_directory_entry(fs, path, &entry, &dir_cluster, &dir_offset);
    terminal_printf("  find_directory_entry result: %d\n", find_res);
    if (find_res != FS_SUCCESS) { return NULL; } // Error or not found

    terminal_write("  Entry found. Allocating vnode/context...\n");
    vnode_t *vnode = (vnode_t *)kmalloc(sizeof(vnode_t));
    terminal_printf("  kmalloc vnode: 0x%x\n", (uintptr_t)vnode); if (!vnode) return NULL;

    fat_file_context_t *file_ctx = (fat_file_context_t *)kmalloc(sizeof(fat_file_context_t));
    terminal_printf("  kmalloc file_ctx: 0x%x\n", (uintptr_t)file_ctx); if (!file_ctx) { kfree(vnode, sizeof(vnode_t)); return NULL; }

    file_ctx->fs = fs;
    file_ctx->first_cluster = (((uint32_t)entry.first_cluster_high) << 16) | entry.first_cluster_low;
    file_ctx->current_cluster = file_ctx->first_cluster;
    file_ctx->file_size = entry.file_size;
    file_ctx->dir_entry_cluster = dir_cluster; // Store entry location
    file_ctx->dir_entry_offset = dir_offset;
    file_ctx->dirty = false;

    vnode->data = file_ctx; vnode->fs_driver = &fat_vfs_driver;
    terminal_printf("  Returning vnode: 0x%x\n", (uintptr_t)vnode);
    return vnode;
}

/**
 * fat_read_internal (Implemented)
 */
static int fat_read_internal(file_t *file, void *buf, size_t len) {
    if (!file || !file->vnode || !file->vnode->data || !buf) return -FS_ERR_INVALID_PARAM;
    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    fat_fs_t *fs = fctx->fs;
    size_t total_read = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;

    // Clamp read length
    if (file->offset >= (off_t)fctx->file_size) return 0; // EOF
    size_t remaining_size = (size_t)fctx->file_size > (size_t)file->offset ? (size_t)fctx->file_size - (size_t)file->offset : 0;
    if (len > remaining_size) len = remaining_size;
    if (len == 0) return 0;

    // Find starting cluster and offset
    uint32_t current_cluster = fctx->first_cluster;
    uint32_t cluster_index = (uint32_t)(file->offset / cluster_size);
    uint32_t offset_in_cluster = (uint32_t)(file->offset % cluster_size);

    // Traverse FAT chain
    for (uint32_t i = 0; i < cluster_index; ++i) {
         uint32_t next_cluster = 0;
         if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) return -FS_ERR_IO;
         // Check EOC based on FAT type
         uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFF8 : 0x0FFFFFF8;
         if (current_cluster < 2 || current_cluster >= eoc_marker) return total_read; // Premature EOC
         current_cluster = next_cluster;
    }
    fctx->current_cluster = current_cluster; // Update context


    // Read loop
    while (total_read < len) {
        uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFF8 : 0x0FFFFFF8;
        if (fctx->current_cluster < 2 || fctx->current_cluster >= eoc_marker) break; // EOC

        size_t bytes_to_read_from_cluster = cluster_size - offset_in_cluster;
        if (bytes_to_read_from_cluster > (len - total_read)) bytes_to_read_from_cluster = len - total_read;

        int cluster_read_result = read_cluster(fs, fctx->current_cluster, offset_in_cluster, (uint8_t *)buf + total_read, bytes_to_read_from_cluster);
        if (cluster_read_result < 0) return cluster_read_result; // Propagate error
        if (cluster_read_result == 0 && bytes_to_read_from_cluster > 0) break; // EOF within cluster?

        total_read += cluster_read_result;
        file->offset += cluster_read_result;
        offset_in_cluster += cluster_read_result;

        // Move to next cluster if needed
        if (offset_in_cluster >= cluster_size && total_read < len) {
            uint32_t next_cluster = 0;
            if (fat_get_next_cluster(fs, fctx->current_cluster, &next_cluster) != 0) return -FS_ERR_IO;
            fctx->current_cluster = next_cluster;
            offset_in_cluster = 0;
        }
    }
    return total_read;
}


/**
 * fat_write_internal (Basic Implementation - Appending/Overwriting)
 */
static int fat_write_internal(file_t *file, const void *buf, size_t len) {
    if (!file || !file->vnode || !file->vnode->data || !buf) return -FS_ERR_INVALID_PARAM;
    if (len == 0) return 0;

    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    fat_fs_t *fs = fctx->fs;
    size_t total_written = 0;
    uint32_t cluster_size = fs->boot_sector.bytes_per_sector * fs->boot_sector.sectors_per_cluster;
    if (cluster_size == 0) return -FS_ERR_INVALID_FORMAT;
    if (!fs->fat_table) { if(load_fat_table(fs)!=0) return -FS_ERR_IO; } // Ensure FAT loaded

    // Determine EOC marker based on FAT type
    uint32_t eoc_marker = (fs->type == FAT_TYPE_FAT16) ? 0xFFFF : 0x0FFFFFFF; // Use actual EOC values

    // Find starting cluster and offset
    uint32_t current_cluster = fctx->first_cluster;
    uint32_t cluster_index = (uint32_t)(file->offset / cluster_size);
    uint32_t offset_in_cluster = (uint32_t)(file->offset % cluster_size);

    // Traverse or extend FAT chain
    if (current_cluster < 2) { // Need to allocate first cluster
        if (file->offset != 0) return -FS_ERR_INVALID_PARAM; // Cannot seek then write to empty file yet
        uint32_t free_cluster = find_free_cluster(fs); if (free_cluster == 0) return -FS_ERR_NO_SPACE;
        fctx->first_cluster = current_cluster = free_cluster;
        fat_set_cluster_entry(fs, current_cluster, eoc_marker);
        fctx->dirty = true; // Mark context dirty to update dir entry later
        // TODO: Need to store location of dir entry in fctx during open to update it!
        terminal_write("[FAT write] TODO: Update dir entry with first cluster.\n");
        flush_fat_table(fs); // Flush FAT change
    } else { // Traverse existing chain
        for (uint32_t i = 0; i < cluster_index; ++i) {
            uint32_t next_cluster = 0;
            if (fat_get_next_cluster(fs, current_cluster, &next_cluster) != 0) return -FS_ERR_IO;
            if (next_cluster >= eoc_marker) { // End of chain, need to extend
                 uint32_t free_cluster = find_free_cluster(fs); if (free_cluster == 0) { flush_fat_table(fs); return total_written > 0 ? total_written : -FS_ERR_NO_SPACE; }
                 fat_set_cluster_entry(fs, current_cluster, free_cluster);
                 fat_set_cluster_entry(fs, free_cluster, eoc_marker);
                 flush_fat_table(fs); next_cluster = free_cluster; fctx->dirty = true;
            }
            current_cluster = next_cluster;
        }
    }
    fctx->current_cluster = current_cluster;

    // Write loop
    while (total_written < len) {
        if (fctx->current_cluster < 2) return -FS_ERR_CORRUPT; // Should have been allocated

        size_t bytes_to_write = cluster_size - offset_in_cluster;
        if (bytes_to_write > (len - total_written)) bytes_to_write = len - total_written;

        int write_res = write_cluster(fs, fctx->current_cluster, offset_in_cluster, (const uint8_t *)buf + total_written, bytes_to_write);
        if (write_res < 0) { flush_fat_table(fs); return write_res; }
        if ((size_t)write_res != bytes_to_write) { /* Handle short write? */ total_written += write_res; file->offset += write_res; break; }

        total_written += bytes_to_write;
        file->offset += bytes_to_write;
        offset_in_cluster += bytes_to_write;
        fctx->dirty = true; // Mark dirty on successful write

        // Update file size in context
        if (file->offset > (off_t)fctx->file_size) {
            fctx->file_size = (uint32_t)file->offset;
        }

        // Allocate next cluster if needed
        if (offset_in_cluster >= cluster_size && total_written < len) {
             uint32_t next_cluster = 0;
             fat_get_next_cluster(fs, fctx->current_cluster, &next_cluster);
             if (next_cluster >= eoc_marker) { // End of chain, allocate new
                  uint32_t free_cluster = find_free_cluster(fs); if (free_cluster == 0) { flush_fat_table(fs); return total_written > 0 ? total_written : -FS_ERR_NO_SPACE; }
                  fat_set_cluster_entry(fs, fctx->current_cluster, free_cluster);
                  fat_set_cluster_entry(fs, free_cluster, eoc_marker);
                  flush_fat_table(fs); next_cluster = free_cluster;
             }
             fctx->current_cluster = next_cluster;
             offset_in_cluster = 0;
        }
    }

    flush_fat_table(fs); // Flush FAT changes at end of operation
    return total_written;
}

/**
 * fat_close_internal (Implemented with Dir Entry Update TODO)
 */
static int fat_close_internal(file_t *file) {
    if (!file || !file->vnode || !file->vnode->data) return -FS_ERR_INVALID_PARAM;
    fat_file_context_t *fctx = (fat_file_context_t *)file->vnode->data;
    fat_fs_t *fs = fctx->fs;

    if (fctx->dirty) {
        terminal_write("[FAT] fat_close: File is dirty. Attempting to update directory entry...\n");
        // --- Update Directory Entry ---
        // This is complex: requires reading the directory cluster/sector,
        // finding the entry by offset, updating size/timestamps, marking buffer dirty.
        // Requires fctx->dir_entry_cluster and fctx->dir_entry_offset to be stored correctly during open.

        // 1. Calculate LBA and offset within sector for the directory entry
        size_t sector_size = fs->boot_sector.bytes_per_sector;
        uint32_t lba;
        size_t offset_in_sector;

        if (fctx->dir_entry_cluster == 0) { // FAT16 Root Directory
            uint32_t root_dir_start_sector = fs->boot_sector.reserved_sector_count + (fs->boot_sector.num_fats * fs->fat_size);
            lba = root_dir_start_sector + (fctx->dir_entry_offset / sector_size);
            offset_in_sector = fctx->dir_entry_offset % sector_size;
        } else { // FAT32 Directory Cluster
            uint32_t sector_in_cluster = fctx->dir_entry_offset / sector_size;
            offset_in_sector = fctx->dir_entry_offset % sector_size;
            lba = fat_cluster_to_lba(fs, fctx->dir_entry_cluster) + sector_in_cluster;
        }

        // 2. Read the directory sector using buffer cache
        terminal_printf("  Updating entry at LBA %u, offset %u\n", lba, offset_in_sector);
        buffer_t *dir_buf = buffer_get(fs->disk.blk_dev.device_name, lba);
        if (!dir_buf) {
            terminal_write("  [ERROR] Failed to get buffer for directory entry sector!\n");
            // Continue with close, but data might be lost/inconsistent
        } else {
             // 3. Modify the entry in the buffer
             fat_dir_entry_t *entry_in_buf = (fat_dir_entry_t *)(dir_buf->data + offset_in_sector);
             entry_in_buf->file_size = fctx->file_size;
             // TODO: Update timestamps (entry_in_buf->write_time, entry_in_buf->write_date)
             // Need current date/time source for this.

             // 4. Mark buffer dirty and release
             buffer_mark_dirty(dir_buf);
             buffer_release(dir_buf);
             terminal_write("  Directory entry marked dirty.\n");

             // 5. Ensure FAT and potentially data buffers are flushed
             flush_fat_table(fs);
             buffer_cache_sync(); // Sync data and the updated dir entry
        }
        fctx->dirty = false; // Clear flag after attempt
    }

    // Free context and vnode
    kfree(file->vnode->data, sizeof(fat_file_context_t));
    kfree(file->vnode, sizeof(vnode_t));
    file->vnode = NULL; // Important for vfs_close

    return FS_SUCCESS;
}

// fat_readdir remains unimplemented
int fat_readdir(fat_fs_t *fs, const char *p, fat_dir_entry_t **e, size_t *c) { return -FS_ERR_NOT_SUPPORTED; }