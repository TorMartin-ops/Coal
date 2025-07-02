/**
 * @file fat_dir.c
 * @brief Directory operations implementation for FAT filesystem driver.
 *
 * Handles VFS operations like open, readdir, unlink, and the core path
 * resolution logic (lookup). Includes helpers for managing directory entries.
 * **MODIFIED:** Corrected fat_open_internal to handle O_CREAT flag.
 */

// --- Includes ---
#include <kernel/fs/fat/fat_dir.h>    // Our declarations
#include <kernel/fs/fat/fat_core.h>   // Core structures (fat_fs_t, vfs_driver_t)
#include <kernel/fs/fat/fat_alloc.h>  // Cluster allocation (fat_allocate_cluster, fat_free_cluster_chain)
                        // *** ADDED: Include fat_create_file declaration ***
#include <kernel/fs/vfs/fs_util.h>    // fs_util_split_path
#include <kernel/fs/fat/fat_utils.h>  // FAT entry access, LBA conversion, name formatting etc.
#include <kernel/fs/fat/fat_lfn.h>    // LFN specific helpers (checksum, reconstruct, generate)
#include <kernel/fs/fat/fat_io.h>     // read_cluster_cached, write_cluster_cached (indirectly via helpers)
#include <kernel/drivers/storage/buffer_cache.h> // Buffer cache access (buffer_get, buffer_release, etc.)
#include <kernel/sync/spinlock.h>   // Locking primitives
#include <kernel/drivers/display/terminal.h>   // Logging (printk equivalent)
#include <kernel/fs/vfs/sys_file.h>   // O_* flags definitions (O_CREAT, O_TRUNC, etc.)
#include <kernel/memory/kmalloc.h>    // Kernel memory allocation
#include <kernel/fs/vfs/fs_errno.h>   // Filesystem error codes (FS_ERR_*)
#include <kernel/fs/vfs/fs_config.h>  // Filesystem limits (FS_MAX_PATH_LENGTH, MAX_FILENAME_LEN)
#include <kernel/core/types.h>      // struct dirent, uint*_t etc.
#include <kernel/lib/string.h>     // memcpy, memcmp, memset, strlen, strchr, strrchr, strtok
#include <kernel/lib/assert.h>     // KERNEL_ASSERT

// --- Local Definitions ---
// (Keep DT_* defines as before)
#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4 // Directory
#define DT_BLK     6
#define DT_REG     8 // Regular file
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14
#endif

// --- Extern Declarations ---
extern vfs_driver_t fat_vfs_driver;

// --- Static Helper Prototypes ---
static void fat_format_short_name_impl(const uint8_t name_8_3[11], char *out_name);

// --- Logging Macros ---
// (Keep logging macros as before)
#ifdef KLOG_LEVEL_DEBUG
#define FAT_DEBUG_LOG(fmt, ...) serial_printf("[fat_dir:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_DEBUG_LOG(fmt, ...) do {} while(0)
#endif
#define FAT_INFO_LOG(fmt, ...)  terminal_printf("[fat_dir:INFO]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_WARN_LOG(fmt, ...)  terminal_printf("[fat_dir:WARN]  (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#define FAT_ERROR_LOG(fmt, ...) terminal_printf("[fat_dir:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

// --- Helper Implementation ---
// (fat_format_short_name_impl remains the same)
static void fat_format_short_name_impl(const uint8_t name_8_3[11], char *out_name) {
    memcpy(out_name, name_8_3, 8);
    int base_len = 8;
    while (base_len > 0 && out_name[base_len - 1] == ' ') base_len--; // Trim trailing spaces from base
    out_name[base_len] = '\0';
    if (name_8_3[8] != ' ') { // If extension exists
        out_name[base_len] = '.';
        base_len++;
        memcpy(out_name + base_len, name_8_3 + 8, 3);
        int ext_len = 3;
        while(ext_len > 0 && out_name[base_len + ext_len - 1] == ' ') ext_len--; // Trim trailing spaces from ext
        out_name[base_len + ext_len] = '\0';
    }
}


/**
 * @brief Opens or creates a file/directory node within the FAT filesystem. (Enhanced Logging)
 * @note This function handles the logic for O_CREAT and O_TRUNC flags.
 * @return Pointer to the allocated vnode on success, NULL on failure.
 */
 vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
 {
     FAT_DEBUG_LOG("Enter: path='%s', flags=0x%x", path ? path : "<NULL>", flags);

     fat_fs_t *fs = (fat_fs_t *)fs_context;
     if (!fs || !path) {
         FAT_ERROR_LOG("Invalid parameters: fs=%p, path=%p", fs, path);
         return NULL;
     }

     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     FAT_DEBUG_LOG("Lock acquired.");

     fat_dir_entry_t entry;
     char lfn_buffer[FAT_MAX_LFN_CHARS]; // Assuming FAT_MAX_LFN_CHARS is defined
     uint32_t entry_dir_cluster = 0;
     uint32_t entry_offset_in_dir = 0;
     int find_res;
     bool exists __attribute__((unused)) = false;
     bool created = false;
     bool truncated = false;
     vnode_t *vnode = NULL;
     fat_file_context_t *file_ctx = NULL;
     int ret_err = FS_SUCCESS; // Assume success initially

     // --- 1. Lookup the path using fat_lookup_path ---
     FAT_DEBUG_LOG("Step 1: Looking up path '%s'...", path);
     find_res = fat_lookup_path(fs, path, &entry, lfn_buffer, sizeof(lfn_buffer),
                                &entry_dir_cluster, &entry_offset_in_dir);
     FAT_DEBUG_LOG("Lookup finished. find_res = %d (%s)", find_res, fs_strerror(find_res));

     // --- 2. Handle Lookup Result ---
     FAT_DEBUG_LOG("Step 2: Handling lookup result (%d)...", find_res);
     if (find_res == FS_SUCCESS) {
         FAT_DEBUG_LOG("Branch: File/Directory Exists.");
         exists = true;
         ret_err = FS_SUCCESS; // Lookup succeeded
         FAT_DEBUG_LOG("Existing entry found: Attr=0x%02x, Size=%lu, Cluster=%lu, DirClu=%lu, DirOff=%lu",
                       entry.attr, (unsigned long)entry.file_size,
                       (unsigned long)fat_get_entry_cluster(&entry),
                       (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);

         bool is_dir = (entry.attr & FAT_ATTR_DIRECTORY);

         // Check O_EXCL flag if creating
         if ((flags & O_CREAT) && (flags & O_EXCL)) {
             FAT_ERROR_LOG("File '%s' exists and O_CREAT|O_EXCL flags were specified.", path);
             ret_err = -FS_ERR_FILE_EXISTS; // Use defined error code
             goto open_fail_locked;
         }
         // Check if trying to write/truncate a directory
         if (is_dir && (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND))) {
             FAT_ERROR_LOG("Cannot open directory '%s' with write/truncate/append flags (0x%x).", path, flags);
             ret_err = -FS_ERR_IS_A_DIRECTORY; // Use defined error code
             goto open_fail_locked;
         }
         // Handle truncation
         if (!is_dir && (flags & O_TRUNC)) {
             if (!(flags & (O_WRONLY | O_RDWR))) {
                 FAT_ERROR_LOG("O_TRUNC specified for '%s' but no write permission requested (flags 0x%x).", path, flags);
                 ret_err = -FS_ERR_PERMISSION_DENIED; // Use defined error code
                 goto open_fail_locked;
             }
             FAT_INFO_LOG("Handling O_TRUNC for existing file '%s', original size=%lu", path, (unsigned long)entry.file_size);
             if (entry.file_size > 0) {
                 int trunc_res = fat_truncate_file(fs, &entry, entry_dir_cluster, entry_offset_in_dir);
                 if (trunc_res != FS_SUCCESS) {
                     FAT_ERROR_LOG("fat_truncate_file failed for '%s', error: %d (%s)", path, trunc_res, fs_strerror(trunc_res));
                     ret_err = trunc_res;
                     goto open_fail_locked;
                 }
                 FAT_DEBUG_LOG("Truncate successful.");
                 truncated = true; // Mark that truncation happened
             } else {
                 FAT_DEBUG_LOG("File already size 0, no truncation needed.");
                 truncated = true; // Mark as conceptually truncated
             }
         }
         // Proceed to allocation stage...

     } else if (find_res == FS_ERR_NOT_FOUND) {
         FAT_DEBUG_LOG("Branch: File/Directory Not Found.");
         exists = false;
         ret_err = find_res; // Keep original error code (-5)

         bool should_create = (flags & O_CREAT);
         FAT_DEBUG_LOG("Checking O_CREAT flag: Present=%d", should_create);
         if (should_create) {
             FAT_INFO_LOG("O_CREAT flag set. Attempting file creation for path '%s'...", path);
             // Call fat_create_file (assuming it's declared in fat_alloc.h)
             int create_res = fat_create_file(fs, path, FAT_ATTR_ARCHIVE, // Default attributes for new file
                                              &entry,                 // Output: created entry info
                                              &entry_dir_cluster,     // Output: cluster where entry was placed
                                              &entry_offset_in_dir);  // Output: offset where entry was placed
             FAT_DEBUG_LOG("fat_create_file returned %d", create_res);

             if (create_res == FS_SUCCESS) {
                 created = true;
                 exists = true; // The file now exists
                 ret_err = FS_SUCCESS; // Reset error status, we succeeded in creating
                 FAT_DEBUG_LOG("O_CREAT successful, new entry info: Size=%lu, Cluster=%lu, DirClu=%lu, DirOff=%lu",
                               (unsigned long)entry.file_size, (unsigned long)fat_get_entry_cluster(&entry),
                               (unsigned long)entry_dir_cluster, (unsigned long)entry_offset_in_dir);
                 // Proceed to allocation stage...
             } else {
                 FAT_ERROR_LOG("fat_create_file failed for '%s', error: %d (%s)", path, create_res, fs_strerror(create_res));
                 ret_err = create_res; // Store the error from creation attempt
                 FAT_DEBUG_LOG("Setting ret_err to %d and jumping to fail path.", ret_err);
                 goto open_fail_locked; // Exit on creation failure
             }
         } else {
             // File not found and O_CREAT not specified
             FAT_DEBUG_LOG("O_CREAT not specified. File not found error (%d) persists.", ret_err);
             goto open_fail_locked;
         }
     } else {
         // Other error during lookup (e.g., I/O error, not a directory in path)
         FAT_DEBUG_LOG("Branch: Other Lookup Error (%d).", find_res);
         FAT_WARN_LOG("Lookup failed for '%s' with unexpected error: %d (%s)", path, find_res, fs_strerror(find_res));
         ret_err = find_res; // Use the error code from lookup
         goto open_fail_locked;
     }

     // --- 3. Allocation & Setup ---
     FAT_DEBUG_LOG("Step 3: Allocating vnode and file context structure...");
     vnode = kmalloc(sizeof(vnode_t));
     file_ctx = kmalloc(sizeof(fat_file_context_t));
     if (!vnode || !file_ctx) {
         FAT_ERROR_LOG("kmalloc failed (vnode=%p, file_ctx=%p). Out of memory.", vnode, file_ctx);
         ret_err = FS_ERR_OUT_OF_MEMORY;
         goto open_fail_locked;
     }
     memset(vnode, 0, sizeof(*vnode));
     memset(file_ctx, 0, sizeof(*file_ctx));
     FAT_DEBUG_LOG("Allocation successful: vnode=%p, file_ctx=%p", vnode, file_ctx);

     // --- 4. Populate context ---
     FAT_DEBUG_LOG("Step 4: Populating file context...");
     uint32_t first_cluster_final = fat_get_entry_cluster(&entry);
     file_ctx->fs                  = fs;
     file_ctx->first_cluster       = first_cluster_final; // Could be 0 if created/truncated
     file_ctx->file_size           = entry.file_size;     // Could be 0 if created/truncated
     file_ctx->dir_entry_cluster   = entry_dir_cluster;
     file_ctx->dir_entry_offset    = entry_offset_in_dir;
     file_ctx->is_directory        = (entry.attr & FAT_ATTR_DIRECTORY);
     file_ctx->dirty               = (created || truncated); // Mark dirty if created or truncated
     file_ctx->readdir_current_cluster = file_ctx->is_directory ? first_cluster_final : 0; // Set readdir start
     if (fs->type != FAT_TYPE_FAT32 && file_ctx->is_directory && file_ctx->first_cluster == 0) {
          file_ctx->readdir_current_cluster = 0; // Adjust for FAT12/16 root
     }
     file_ctx->readdir_current_offset = 0;
     file_ctx->readdir_last_index = (size_t)-1; // Initialize readdir state
     FAT_DEBUG_LOG("Context populated: first_cluster=%lu, size=%lu, is_dir=%d, dirty=%d",
                   (unsigned long)file_ctx->first_cluster, (unsigned long)file_ctx->file_size,
                   file_ctx->is_directory, file_ctx->dirty);

     // --- 5. Link context to Vnode ---
     FAT_DEBUG_LOG("Step 5: Linking context %p to vnode %p...", file_ctx, vnode);
     vnode->data = file_ctx;
     vnode->fs_driver = &fat_vfs_driver; // Assuming fat_vfs_driver is the global driver struct

     // --- Success ---
     FAT_DEBUG_LOG("Step 6: Success Path.");
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     FAT_DEBUG_LOG("Lock released.");
     FAT_INFO_LOG("Open successful: path='%s', vnode=%p, size=%lu", path ? path : "<NULL>", vnode, (unsigned long)file_ctx->file_size);
     return vnode;

 // --- Failure Path ---
 open_fail_locked:
     FAT_DEBUG_LOG("Step F: Failure Path Entered (ret_err=%d).", ret_err);
     FAT_ERROR_LOG("Open failed: path='%s', error=%d (%s)", path ? path : "<NULL>", ret_err, fs_strerror(ret_err)); // Log the final error
     if (vnode) { FAT_DEBUG_LOG("Freeing vnode %p", vnode); kfree(vnode); }
     if (file_ctx) { FAT_DEBUG_LOG("Freeing file_ctx %p", file_ctx); kfree(file_ctx); }
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     FAT_DEBUG_LOG("Lock released.");
     return NULL;
 }

 

// --- Other functions in fat_dir.c (fat_readdir_internal, fat_unlink_internal, fat_find_in_dir, etc.) remain the same ---
// ... (Keep the existing implementations of other functions in this file) ...

// ==========================================================================
// == fat_readdir_internal - Definition should remain here ==
// ==========================================================================
int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index)
{
    FAT_DEBUG_LOG("Enter: dir_file=%p, d_entry_out=%p, entry_index=%lu", dir_file, d_entry_out, (unsigned long)entry_index);

    if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out) {
        FAT_ERROR_LOG("Invalid parameters: dir_file=%p, vnode=%p, data=%p, d_entry_out=%p",
                      dir_file, dir_file ? dir_file->vnode : NULL,
                      dir_file && dir_file->vnode ? dir_file->vnode->data : NULL, d_entry_out);
        return FS_ERR_INVALID_PARAM;
    }

    fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
    if (!fctx->fs || !fctx->is_directory) {
        FAT_ERROR_LOG("Context error: fs=%p, is_directory=%d. Not a valid directory context.", fctx->fs, fctx->is_directory);
        return FS_ERR_NOT_A_DIRECTORY;
    }
    fat_fs_t *fs = fctx->fs;
    FAT_DEBUG_LOG("Context valid: fs=%p, first_cluster=%lu", fs, (unsigned long)fctx->first_cluster);

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);

    // --- State Management ---
    FAT_DEBUG_LOG("Checking readdir state: requested_idx=%lu, last_idx=%lu, current_cluster=%lu, current_offset=%lu",
                  (unsigned long)entry_index, (unsigned long)fctx->readdir_last_index,
                  (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);

    if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
        // ... (reset logic unchanged) ...
        if (entry_index != 0) {
             FAT_DEBUG_LOG("Implicit reset: requested index %lu <= last index %lu.", (unsigned long)entry_index, (unsigned long)fctx->readdir_last_index);
        } else {
             FAT_DEBUG_LOG("Resetting scan to start (index 0).");
        }
        fctx->readdir_current_cluster = fctx->first_cluster;
        if (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0) {
            FAT_DEBUG_LOG("Adjusting start cluster for FAT12/16 root directory.");
            fctx->readdir_current_cluster = 0;
        }
        fctx->readdir_current_offset = 0;
        fctx->readdir_last_index = (size_t)-1;
        FAT_DEBUG_LOG("Scan reset: start_cluster=%lu, start_offset=0, last_index=%lu",
                      (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_last_index);

    } else if (entry_index != fctx->readdir_last_index + 1) {
        FAT_WARN_LOG("Non-sequential index requested (%lu requested, %lu expected). Seeking not implemented, failing.", // Now uses DEBUG log
                     (unsigned long)entry_index, (unsigned long)(fctx->readdir_last_index + 1));
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        return FS_ERR_INVALID_PARAM;
    }

    // --- Allocate buffer --- (Code unchanged)
     uint8_t *sector_buffer = kmalloc(fs->bytes_per_sector);
    if (!sector_buffer) {
        FAT_ERROR_LOG("Failed to allocate %u bytes for sector buffer.", fs->bytes_per_sector);
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        return FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p (%u bytes).", sector_buffer, fs->bytes_per_sector);

    // --- Init scan variables --- (Code unchanged)
    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
    int lfn_count = 0;
    size_t current_logical_index = fctx->readdir_last_index + 1;
    int ret = FS_ERR_NOT_FOUND;

    FAT_DEBUG_LOG("Starting scan for logical index %lu (expecting index %lu). Current pos: cluster=%lu, offset=%lu",
                  (unsigned long)current_logical_index, (unsigned long)entry_index,
                  (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);


    // --- Directory Scanning Loop ---
    while (true) { // (Loop logic largely unchanged, only logging format adjusted)
        FAT_DEBUG_LOG("Loop iteration: target_idx=%lu, current_logical_idx=%lu, cluster=%lu, offset=%lu",
                      (unsigned long)entry_index, (unsigned long)current_logical_index,
                      (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);

        bool is_fat12_16_root = (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0);
        // ... (End of chain / End of root dir checks unchanged) ...
         if (!is_fat12_16_root && fctx->readdir_current_cluster >= fs->eoc_marker) {
            FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu).",
                           (unsigned long)fctx->readdir_current_cluster, (unsigned long)fs->eoc_marker);
            ret = FS_ERR_NOT_FOUND;
            break;
        }
        if (is_fat12_16_root && fctx->readdir_current_offset >= (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu).",
                          (unsigned long)fctx->readdir_current_offset, (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
             ret = FS_ERR_NOT_FOUND;
             break;
        }

        // Calculate sector position
        uint32_t sec_size = fs->bytes_per_sector;
        uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
        size_t   offset_in_sector       = fctx->readdir_current_offset % sec_size;
        size_t   entries_per_sector     = sec_size / sizeof(fat_dir_entry_t);
        size_t   entry_index_in_sector  = offset_in_sector / sizeof(fat_dir_entry_t);

        FAT_DEBUG_LOG("Reading sector: chain_offset=%lu, offset_in_sec=%lu, entry_idx_in_sec=%lu",
                      (unsigned long)sector_offset_in_chain, (unsigned long)offset_in_sector, (unsigned long)entry_index_in_sector);

        // Read sector
        int read_res = read_directory_sector(fs, fctx->readdir_current_cluster,
                                             sector_offset_in_chain, sector_buffer);
        if (read_res != FS_SUCCESS) {
            FAT_ERROR_LOG("read_directory_sector failed with error %d.", read_res);
            ret = read_res;
            break;
        }
        FAT_DEBUG_LOG("Sector read successful.");

        // Iterate through entries
        for (size_t e_i = entry_index_in_sector; e_i < entries_per_sector; e_i++)
        {
             // ... (Entry processing logic unchanged, only logging formats adjusted) ...
            fat_dir_entry_t *dent = (fat_dir_entry_t*)(sector_buffer + e_i * sizeof(fat_dir_entry_t));
            FAT_DEBUG_LOG("Processing entry at sector_offset %lu: Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)(e_i * sizeof(fat_dir_entry_t)), dent->name[0], dent->attr);

            fctx->readdir_current_offset += sizeof(fat_dir_entry_t);

            if (dent->name[0] == FAT_DIR_ENTRY_UNUSED) { /* ... */ ret = FS_ERR_NOT_FOUND; goto readdir_done;}
            if (dent->name[0] == FAT_DIR_ENTRY_DELETED || dent->name[0] == FAT_DIR_ENTRY_KANJI) { /* ... */ lfn_count = 0; continue;}
            if ((dent->attr & FAT_ATTR_VOLUME_ID) && !(dent->attr & FAT_ATTR_LONG_NAME)) { /* ... */ lfn_count = 0; continue;}

            if ((dent->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                // ... (LFN handling unchanged, just log format fixed) ...
                 fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                FAT_DEBUG_LOG("Found LFN entry: Attr=0x%02x, Checksum=0x%02x", lfn_ent->attr, lfn_ent->checksum);
                if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                    lfn_collector[lfn_count++] = *lfn_ent;
                    FAT_DEBUG_LOG("Stored LFN entry %d", lfn_count);
                } else {
                    FAT_WARN_LOG("LFN entry sequence exceeded buffer (%d entries). Discarding LFN.", FAT_MAX_LFN_ENTRIES); // Uses DEBUG
                    lfn_count = 0;
                }
                continue;
            }
            else {
                // --- Found an 8.3 Entry ---
                FAT_DEBUG_LOG("Found 8.3 entry: Name='%.11s', Attr=0x%02x", dent->name, dent->attr);

                if (current_logical_index == entry_index) {
                    FAT_INFO_LOG("Target logical index %lu found!", (unsigned long)entry_index); // Uses DEBUG
                    // ... (Name reconstruction unchanged) ...
                    char final_name[FAT_MAX_LFN_CHARS];
                    final_name[0] = '\0';
                    if (lfn_count > 0) { /* ... checksum check ... */
                        FAT_DEBUG_LOG("Attempting to reconstruct LFN from %d collected entries.", lfn_count);
                        uint8_t expected_sum = fat_calculate_lfn_checksum(dent->name);
                         if (lfn_collector[0].checksum == expected_sum) {
                            fat_reconstruct_lfn(lfn_collector, lfn_count, final_name, sizeof(final_name));
                            if(final_name[0] != '\0') { FAT_DEBUG_LOG("LFN reconstruction successful: '%s'", final_name); }
                            else { FAT_WARN_LOG("LFN reconstruction failed. Using 8.3 name.",0); /* Uses DEBUG */}
                         } else {
                              FAT_WARN_LOG("LFN checksum mismatch. Discarding LFN.",0); // Uses DEBUG
                              lfn_count = 0;
                         }
                    } else { FAT_DEBUG_LOG("No preceding LFN entries found."); }
                    if (final_name[0] == '\0') {
                         fat_format_short_name_impl(dent->name, final_name); // Use helper
                         FAT_DEBUG_LOG("Using formatted 8.3 name: '%s'", final_name);
                     }


                    // Populate output dirent
                    FAT_DEBUG_LOG("Populating output dirent: name='%s', cluster=%lu, attr=0x%02x",
                                  final_name, (unsigned long)fat_get_entry_cluster(dent), dent->attr);
                    // ... (strncpy, set ino, set type unchanged) ...
                     strncpy(d_entry_out->d_name, final_name, sizeof(d_entry_out->d_name) - 1);
                    d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0';
                    d_entry_out->d_ino = fat_get_entry_cluster(dent);
                    d_entry_out->d_type = (dent->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;


                    // Update state
                    fctx->readdir_last_index = entry_index;
                    FAT_DEBUG_LOG("Updated context state: last_index=%lu, current_cluster=%lu, current_offset=%lu",
                                  (unsigned long)fctx->readdir_last_index, (unsigned long)fctx->readdir_current_cluster,
                                  (unsigned long)fctx->readdir_current_offset);
                    ret = FS_SUCCESS;
                    goto readdir_done;
                }

                // Not the target entry
                FAT_DEBUG_LOG("Logical index %lu does not match target %lu. Incrementing logical index.",
                              (unsigned long)current_logical_index, (unsigned long)entry_index);
                current_logical_index++;
                lfn_count = 0;
            }
        } // End loop through entries in sector

        // --- Move to next sector/cluster --- (Logic unchanged)
         if (!is_fat12_16_root && (fctx->readdir_current_offset % fs->cluster_size_bytes == 0) && fctx->readdir_current_offset > 0)
        {
             FAT_DEBUG_LOG("End of cluster %lu reached (offset %lu). Finding next cluster.",
                           (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, fctx->readdir_current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) { /* ... error handling ... */ ret = get_next_res; break;}
            FAT_DEBUG_LOG("Next cluster in chain is %lu.", (unsigned long)next_c);
            if (next_c >= fs->eoc_marker) { /* ... end of chain ... */ ret = FS_ERR_NOT_FOUND; break;}
            fctx->readdir_current_cluster = next_c;
            fctx->readdir_current_offset = 0;
            FAT_DEBUG_LOG("Moved to next cluster: cluster=%lu, offset=0", (unsigned long)fctx->readdir_current_cluster);
        }

    } // End while(true) loop

// --- Cleanup and Return ---
readdir_done:
    FAT_DEBUG_LOG("Exiting: Releasing lock, freeing buffer %p, returning status %d (%s).",
                   sector_buffer, ret, fs_strerror(ret));
    kfree(sector_buffer);
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    return ret;
}

// ==========================================================================
// == fat_unlink_internal - Definition should remain here ==
// ==========================================================================
int fat_unlink_internal(void *fs_context, const char *path)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs || !path) return FS_ERR_INVALID_PARAM;

     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     int ret = FS_SUCCESS; // Assume success initially

     // 1. Split path into parent directory path and final component name
     char parent_path[FS_MAX_PATH_LENGTH];
     char component_name[MAX_FILENAME_LEN + 1];
     if (fs_util_split_path(path, parent_path, sizeof(parent_path), component_name, sizeof(component_name)) != 0) {
         ret = FS_ERR_NAMETOOLONG;
         goto unlink_fail_locked;
     }
      if (strlen(component_name) == 0 || strcmp(component_name, ".") == 0 || strcmp(component_name, "..") == 0) {
         ret = FS_ERR_INVALID_PARAM; // Cannot unlink empty name, ".", or ".."
         goto unlink_fail_locked;
     }

     // 2. Lookup the parent directory
     fat_dir_entry_t parent_entry;
     uint32_t parent_entry_dir_cluster, parent_entry_offset; // Not needed here
     int parent_res = fat_lookup_path(fs, parent_path, &parent_entry, NULL, 0,
                                      &parent_entry_dir_cluster, &parent_entry_offset);
     if (parent_res != FS_SUCCESS) {
         ret = parent_res; // Parent not found or other error
         goto unlink_fail_locked;
     }
     if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
         ret = FS_ERR_NOT_A_DIRECTORY; // Parent path is not a directory
         goto unlink_fail_locked;
     }
     uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
     if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_path, "/") == 0) {
          parent_cluster = 0; // Handle FAT12/16 root
     }

     // 3. Find the actual entry within the parent directory using the component name
     fat_dir_entry_t entry_to_delete;
     uint32_t entry_offset;            // Offset of the 8.3 entry
     uint32_t first_lfn_offset = (uint32_t)-1; // Offset of the first LFN entry (if any)
     int find_res = fat_find_in_dir(fs, parent_cluster, component_name,
                                    &entry_to_delete, NULL, 0, // Don't need LFN name buffer here
                                    &entry_offset, &first_lfn_offset);

     if (find_res != FS_SUCCESS) {
         ret = find_res; // File/component not found in parent directory, or I/O error
         goto unlink_fail_locked;
     }

     // --- Perform Checks ---
     // Check if it's a directory - cannot unlink directories with this function
     if (entry_to_delete.attr & FAT_ATTR_DIRECTORY) {
         ret = FS_ERR_IS_A_DIRECTORY; // Use rmdir instead
         goto unlink_fail_locked;
     }
     // Check if it's read-only
     if (entry_to_delete.attr & FAT_ATTR_READ_ONLY) {
         ret = FS_ERR_PERMISSION_DENIED;
         goto unlink_fail_locked;
     }

     // --- Free cluster chain ---
     uint32_t file_cluster = fat_get_entry_cluster(&entry_to_delete);
     if (file_cluster >= 2) { // Only free if file actually has clusters allocated
         int free_res = fat_free_cluster_chain(fs, file_cluster);
         if (free_res != FS_SUCCESS) {
             FAT_WARN_LOG("Warning: Error freeing cluster chain for '%s' (err %d).", path, free_res); // Use WARN
             // Non-fatal? Continue to mark entry deleted anyway, but report error?
             ret = free_res; // Report the error, but proceed with marking entry deleted
             // Alternatively: goto unlink_fail_locked; // Make cluster free failure fatal
         }
     }

     // --- Mark directory entries as deleted ---
     size_t num_entries_to_mark = 1; // Start with the 8.3 entry
     uint32_t mark_start_offset = entry_offset; // Offset of the 8.3 entry

     // If LFN entries preceded the 8.3 entry, mark them too
     if (first_lfn_offset != (uint32_t)-1 && first_lfn_offset < entry_offset) {
         num_entries_to_mark = ((entry_offset - first_lfn_offset) / sizeof(fat_dir_entry_t)) + 1;
         mark_start_offset = first_lfn_offset; // Start marking from the first LFN entry
     }

     int mark_res = mark_directory_entries_deleted(fs, parent_cluster,
                                                 mark_start_offset,
                                                 num_entries_to_mark,
                                                 FAT_DIR_ENTRY_DELETED); // Use 0xE5 marker
     if (mark_res != FS_SUCCESS) {
         FAT_ERROR_LOG("Error marking directory entry deleted for '%s' (err %d).", path, mark_res);
         ret = mark_res; // Report marking error
         goto unlink_fail_locked; // Make marking failure fatal
     }

     // --- Flush changes ---
     buffer_cache_sync(); // Ensure deletion markers are written to disk

     FAT_INFO_LOG("Successfully unlinked '%s'.", path); // Use INFO
     // Fall through to return FS_SUCCESS unless an error occurred and wasn't fatal

 unlink_fail_locked:
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return ret; // Return final status
 }


// ==========================================================================
// == fat_find_in_dir - Definition should remain here ==
// ==========================================================================
int fat_find_in_dir(fat_fs_t *fs,
                   uint32_t dir_cluster,
                   const char *component,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_offset_in_dir_out,
                   uint32_t *first_lfn_offset_out)
{
    KERNEL_ASSERT(fs != NULL && component != NULL && entry_out != NULL && entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_find_in_dir for required arguments");
    KERNEL_ASSERT(strlen(component) > 0, "Component name cannot be empty");

    // Use %lu for unsigned long arguments
    FAT_DEBUG_LOG("Enter: Searching for '%s' in dir_cluster %lu", component, (unsigned long)dir_cluster);

    uint32_t current_cluster = dir_cluster;
    bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
    uint32_t current_byte_offset = 0;

    if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
    if (first_lfn_offset_out) *first_lfn_offset_out = (uint32_t)-1;

    uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
    if (!sector_data) {
        FAT_ERROR_LOG("ERROR: Failed to allocate sector buffer (%u bytes)", fs->bytes_per_sector);
        return FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p", sector_data);

    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
    int lfn_count = 0;
    uint32_t current_lfn_start_offset = (uint32_t)-1;
    int ret = FS_ERR_NOT_FOUND; // Initialize to NOT_FOUND

    while (true) {
        FAT_DEBUG_LOG("Loop: current_cluster=%lu, current_byte_offset=%lu", (unsigned long)current_cluster, (unsigned long)current_byte_offset);
        if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
            FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu).", (unsigned long)current_cluster, (unsigned long)fs->eoc_marker);
            ret = FS_ERR_NOT_FOUND;
            break;
        }
         if (scanning_fixed_root && current_byte_offset >= (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) {
             FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu).",
                           (unsigned long)current_byte_offset, (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
             ret = FS_ERR_NOT_FOUND;
             break;
         }

        uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);

        FAT_DEBUG_LOG("Reading sector: chain_offset=%lu", (unsigned long)sector_offset_in_chain);
        int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
        if (read_res != FS_SUCCESS) {
            FAT_ERROR_LOG("ERROR: read_directory_sector failed (err %d)", read_res);
            ret = read_res;
            break;
        }
        FAT_DEBUG_LOG("Sector read success. Processing %lu entries...", (unsigned long)entries_per_sector);

        for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
            fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
            uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));

            FAT_DEBUG_LOG("Entry %lu (abs_offset %lu): Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)e_idx, (unsigned long)entry_abs_offset, de->name[0], de->attr);

            if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                FAT_DEBUG_LOG("Found UNUSED entry marker (0x00). End of directory.");
                ret = FS_ERR_NOT_FOUND;
                goto find_done;
            }
            if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) { lfn_count = 0; current_lfn_start_offset = (uint32_t)-1; continue; }
            if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) { lfn_count = 0; current_lfn_start_offset = (uint32_t)-1; continue; }

            if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)de;
                if(lfn_count==0) current_lfn_start_offset=entry_abs_offset;
                if(lfn_count<FAT_MAX_LFN_ENTRIES) lfn_collector[lfn_count++]=*lfn_ent;
                else {lfn_count=0; current_lfn_start_offset=(uint32_t)-1;}
            } else {
                bool match=false;
                char recon_lfn[FAT_MAX_LFN_CHARS];
                if(lfn_count > 0){
                    uint8_t sum=fat_calculate_lfn_checksum(de->name);
                    if(lfn_collector[0].checksum == sum){
                        fat_reconstruct_lfn(lfn_collector,lfn_count,recon_lfn,sizeof(recon_lfn));
                        if(fat_compare_lfn(component, recon_lfn)==0) match=true;
                    } else { lfn_count=0; current_lfn_start_offset=(uint32_t)-1; }
                }
                if(!match){ if(fat_compare_8_3(component, de->name)==0) match=true; }

                if (match) {
                    memcpy(entry_out, de, sizeof(fat_dir_entry_t));
                    *entry_offset_in_dir_out = entry_abs_offset;
                    if (first_lfn_offset_out) { *first_lfn_offset_out = current_lfn_start_offset; }
                    ret = FS_SUCCESS;
                    goto find_done;
                }
                lfn_count = 0; current_lfn_start_offset = (uint32_t)-1;
            }
        } // End entry loop

        current_byte_offset += fs->bytes_per_sector;
        FAT_DEBUG_LOG("Advanced to next sector offset: %lu", (unsigned long)current_byte_offset);

        if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
            FAT_DEBUG_LOG("End of cluster %lu reached. Finding next...", (unsigned long)current_cluster);
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) { ret = get_next_res; break; }
            FAT_DEBUG_LOG("Next cluster in chain: %lu", (unsigned long)next_c);
            if (next_c >= fs->eoc_marker) { ret = FS_ERR_NOT_FOUND; break; }
            current_cluster = next_c;
            current_byte_offset = 0;
        }
    } // End while loop

find_done:
    FAT_DEBUG_LOG("Exit: Freeing buffer %p, returning status %d (%s)",
                  sector_data, ret, fs_strerror(ret));
    kfree(sector_data);
    return ret;
}


// ==========================================================================
// == fat_lookup_path - Definition should remain here ==
// ==========================================================================
int fat_lookup_path(fat_fs_t *fs, const char *path,
                   fat_dir_entry_t *entry_out,
                   char *lfn_out, size_t lfn_max_len,
                   uint32_t *entry_dir_cluster_out,
                   uint32_t *entry_offset_in_dir_out)
{
    KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && entry_dir_cluster_out != NULL && entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_lookup_path for required arguments");

    FAT_DEBUG_LOG("Received path from VFS: '%s'", path);

    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        FAT_DEBUG_LOG("Handling as root directory.");
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->attr = FAT_ATTR_DIRECTORY;
        *entry_offset_in_dir_out = 0;
        if (fs->type == FAT_TYPE_FAT32) {
            entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
            entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
            *entry_dir_cluster_out = 0;
        } else {
            entry_out->first_cluster_low  = 0;
            entry_out->first_cluster_high = 0;
            *entry_dir_cluster_out = 0;
        }
        if (lfn_out && lfn_max_len > 0) { strncpy(lfn_out, "/", lfn_max_len -1); lfn_out[lfn_max_len - 1] = '\0'; }
        return FS_SUCCESS;
    }

    char *path_copy = kmalloc(strlen(path) + 1);
    if (!path_copy) return FS_ERR_OUT_OF_MEMORY;
    strcpy(path_copy, path);

    char *component = strtok(path_copy, "/");
    uint32_t current_dir_cluster = (fs->type == FAT_TYPE_FAT32) ? fs->root_cluster : 0;
    fat_dir_entry_t current_entry;
    memset(&current_entry, 0, sizeof(current_entry));
    current_entry.attr = FAT_ATTR_DIRECTORY;
    uint32_t previous_dir_cluster = 0;
    int ret = FS_ERR_NOT_FOUND;

    while (component != NULL) {
        if (strcmp(component, ".") == 0) { component = strtok(NULL, "/"); continue; }
        if (strcmp(component, "..") == 0) { ret = FS_ERR_NOT_SUPPORTED; goto lookup_done; }

        previous_dir_cluster = current_dir_cluster;
        uint32_t component_entry_offset;
        int find_comp_res = fat_find_in_dir(fs, current_dir_cluster, component,
                                             &current_entry, lfn_out, lfn_max_len,
                                             &component_entry_offset, NULL);
        if (find_comp_res != FS_SUCCESS) { ret = find_comp_res; goto lookup_done; }

        char* next_component = strtok(NULL, "/");
        if (next_component == NULL) {
            memcpy(entry_out, &current_entry, sizeof(*entry_out));
            *entry_dir_cluster_out = previous_dir_cluster;
            *entry_offset_in_dir_out = component_entry_offset;
            ret = FS_SUCCESS;
            goto lookup_done;
        }

        if (!(current_entry.attr & FAT_ATTR_DIRECTORY)) { ret = FS_ERR_NOT_A_DIRECTORY; goto lookup_done; }
        current_dir_cluster = fat_get_entry_cluster(&current_entry);
        if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0 && previous_dir_cluster != 0) { ret = FS_ERR_INVALID_FORMAT; goto lookup_done; }
        component = next_component;
    }

lookup_done:
    FAT_DEBUG_LOG("Exit: Path='%s', returning status %d (%s)", path, ret, fs_strerror(ret));
    kfree(path_copy);
    return ret;
}


// ==========================================================================
// == read_directory_sector - Definition should remain here ==
// ==========================================================================
int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                          uint32_t sector_offset_in_chain,
                          uint8_t* buffer)
{
    // (Keep existing implementation)
    KERNEL_ASSERT(fs != NULL && buffer != NULL, "FS context and buffer cannot be NULL in read_directory_sector");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector in FS context");

    uint32_t lba;
    int ret = FS_SUCCESS;

    if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
        KERNEL_ASSERT(fs->root_dir_sectors > 0, "FAT12/16 root dir sector count is zero");
        if (sector_offset_in_chain >= fs->root_dir_sectors) return FS_ERR_NOT_FOUND;
        lba = fs->root_dir_start_lba + sector_offset_in_chain;
    } else if (cluster >= 2) {
        KERNEL_ASSERT(fs->sectors_per_cluster > 0, "Invalid sectors_per_cluster in FS context");
        uint32_t current_scan_cluster = cluster;
        uint32_t sectors_per_cluster = fs->sectors_per_cluster;
        uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
        uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;

        for (uint32_t i = 0; i < cluster_hop_count; i++) {
            uint32_t next_cluster;
            ret = fat_get_next_cluster(fs, current_scan_cluster, &next_cluster);
            if (ret != FS_SUCCESS) return ret;
            if (next_cluster >= fs->eoc_marker) return FS_ERR_NOT_FOUND;
            current_scan_cluster = next_cluster;
        }
        uint32_t cluster_start_lba = fat_cluster_to_lba(fs, current_scan_cluster);
        if (cluster_start_lba == 0) return FS_ERR_IO;
        lba = cluster_start_lba + sector_in_final_cluster;
    } else {
        return FS_ERR_INVALID_PARAM;
    }

    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) return FS_ERR_IO;
    memcpy(buffer, b->data, fs->bytes_per_sector);
    buffer_release(b);
    return FS_SUCCESS;
}

// ==========================================================================
// == update_directory_entry - Definition should remain here ==
// ==========================================================================
int update_directory_entry(fat_fs_t *fs,
                           uint32_t dir_cluster,
                           uint32_t dir_offset,
                           const fat_dir_entry_t *new_entry)
{
    // (Keep existing implementation)
    KERNEL_ASSERT(fs != NULL && new_entry != NULL, "FS context and new entry cannot be NULL in update_directory_entry");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t sector_size = fs->bytes_per_sector;
    uint32_t sector_offset_in_chain = dir_offset / sector_size;
    size_t offset_in_sector = dir_offset % sector_size;

    KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Directory entry offset misaligned");
    KERNEL_ASSERT(offset_in_sector + sizeof(fat_dir_entry_t) <= sector_size, "Directory entry update crosses sector boundary");

    uint32_t lba;
    int ret = FS_SUCCESS;
    if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         if (sector_offset_in_chain >= fs->root_dir_sectors) return FS_ERR_INVALID_PARAM;
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
    } else if (dir_cluster >= 2) {
         uint32_t current_cluster = dir_cluster;
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
             uint32_t next_cluster;
             ret = fat_get_next_cluster(fs, current_cluster, &next_cluster);
             if (ret != FS_SUCCESS) return ret;
             if (next_cluster >= fs->eoc_marker) return FS_ERR_INVALID_PARAM;
             current_cluster = next_cluster;
         }
         uint32_t cluster_lba = fat_cluster_to_lba(fs, current_cluster);
         if (cluster_lba == 0) return FS_ERR_IO;
         lba = cluster_lba + sector_in_final_cluster;
    } else {
        return FS_ERR_INVALID_PARAM;
    }

    buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
    if (!b) return FS_ERR_IO;
    memcpy(b->data + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
    buffer_mark_dirty(b);
    buffer_release(b);
    return FS_SUCCESS;
}

// ==========================================================================
// == mark_directory_entries_deleted - Definition should remain here ==
// ==========================================================================
int mark_directory_entries_deleted(fat_fs_t *fs,
                                   uint32_t dir_cluster,
                                   uint32_t first_entry_offset,
                                   size_t num_entries,
                                   uint8_t marker)
{
    // (Keep existing implementation)
    KERNEL_ASSERT(fs != NULL && num_entries > 0, "FS context must be valid and num_entries > 0");
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t sector_size = fs->bytes_per_sector;
    int result = FS_SUCCESS;
    size_t entries_marked = 0;
    uint32_t current_offset = first_entry_offset;

    while (entries_marked < num_entries) {
        uint32_t sector_offset_in_chain = current_offset / sector_size;
        size_t offset_in_sector = current_offset % sector_size;
        KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Entry offset misaligned in mark");

        uint32_t lba;
         if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
             if (sector_offset_in_chain >= fs->root_dir_sectors) { result = FS_ERR_INVALID_PARAM; break; }
             lba = fs->root_dir_start_lba + sector_offset_in_chain;
         } else if (dir_cluster >= 2) {
             uint32_t current_data_cluster = dir_cluster;
             uint32_t sectors_per_cluster = fs->sectors_per_cluster;
             uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
             uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
             for (uint32_t i = 0; i < cluster_hop_count; i++) {
                 uint32_t next_cluster;
                 result = fat_get_next_cluster(fs, current_data_cluster, &next_cluster);
                 if (result != FS_SUCCESS) goto mark_fail;
                 if (next_cluster >= fs->eoc_marker) { result = FS_ERR_INVALID_PARAM; goto mark_fail; }
                 current_data_cluster = next_cluster;
             }
             uint32_t cluster_lba = fat_cluster_to_lba(fs, current_data_cluster);
             if (cluster_lba == 0) { result = FS_ERR_IO; break; }
             lba = cluster_lba + sector_in_final_cluster;
         } else { result = FS_ERR_INVALID_PARAM; break; }

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
        if (!b) { result = FS_ERR_IO; break; }

        bool buffer_dirtied = false;
        while (entries_marked < num_entries && offset_in_sector < sector_size) {
            fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(b->data + offset_in_sector);
            entry_ptr->name[0] = marker;
            buffer_dirtied = true;
            offset_in_sector += sizeof(fat_dir_entry_t);
            current_offset   += sizeof(fat_dir_entry_t);
            entries_marked++;
        }
        if (buffer_dirtied) buffer_mark_dirty(b);
        buffer_release(b);
        if (result != FS_SUCCESS) break;
    }
mark_fail:
    return result;
}

// ==========================================================================
// == write_directory_entries - Definition should remain here ==
// ==========================================================================
int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                            uint32_t dir_offset,
                            const void *entries_buf,
                            size_t num_entries)
{
    // (Keep existing implementation)
    KERNEL_ASSERT(fs != NULL && entries_buf != NULL, "FS context and entry buffer cannot be NULL in write_directory_entries");
    if (num_entries == 0) return FS_SUCCESS;
    KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");

    size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
    size_t sector_size = fs->bytes_per_sector;
    const uint8_t *src_buf = (const uint8_t *)entries_buf;
    size_t bytes_written = 0;
    int result = FS_SUCCESS;

    while (bytes_written < total_bytes) {
        uint32_t current_abs_offset = dir_offset + (uint32_t)bytes_written;
        uint32_t sector_offset_in_chain = current_abs_offset / sector_size;
        size_t offset_in_sector = current_abs_offset % sector_size;
        KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Write offset misaligned");

        uint32_t lba;
         if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) {
             if (sector_offset_in_chain >= fs->root_dir_sectors) { result = FS_ERR_INVALID_PARAM; break; }
             lba = fs->root_dir_start_lba + sector_offset_in_chain;
         } else if (dir_cluster >= 2) {
             uint32_t current_data_cluster = dir_cluster;
             uint32_t sectors_per_cluster = fs->sectors_per_cluster;
             uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
             uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
             for (uint32_t i = 0; i < cluster_hop_count; i++) {
                 uint32_t next_cluster;
                 result = fat_get_next_cluster(fs, current_data_cluster, &next_cluster);
                 if (result != FS_SUCCESS) goto write_fail;
                 if (next_cluster >= fs->eoc_marker) { result = FS_ERR_INVALID_PARAM; goto write_fail; }
                 current_data_cluster = next_cluster;
             }
             uint32_t cluster_lba = fat_cluster_to_lba(fs, current_data_cluster);
              if (cluster_lba == 0) { result = FS_ERR_IO; break; }
             lba = cluster_lba + sector_in_final_cluster;
         } else { result = FS_ERR_INVALID_PARAM; break; }

        buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
        if (!b) { result = FS_ERR_IO; break; }

        size_t bytes_to_write_this_sector = sector_size - offset_in_sector;
        size_t bytes_remaining_total = total_bytes - bytes_written;
        if (bytes_to_write_this_sector > bytes_remaining_total) {
            bytes_to_write_this_sector = bytes_remaining_total;
        }
        KERNEL_ASSERT(bytes_to_write_this_sector > 0, "Zero bytes to write calculation error");

        memcpy(b->data + offset_in_sector, src_buf + bytes_written, bytes_to_write_this_sector);
        buffer_mark_dirty(b);
        buffer_release(b);
        bytes_written += bytes_to_write_this_sector;
    }
write_fail:
    return result;
}

// ==========================================================================
// == find_free_directory_slot - Definition should remain here ==
// ==========================================================================
int find_free_directory_slot(fat_fs_t    *fs,
                             uint32_t     parent_dir_cluster,
                             size_t       needed_slots,
                             uint32_t    *out_slot_cluster,
                             uint32_t    *out_slot_offset)
{
    FAT_DEBUG_LOG("Enter: Searching for %lu slots in dir_cluster %lu", (unsigned long)needed_slots, (unsigned long)parent_dir_cluster);
    KERNEL_ASSERT(fs && needed_slots && out_slot_cluster && out_slot_offset, "find_free_directory_slot: bad args");

    const bool fixed_root = (fs->type != FAT_TYPE_FAT32 && parent_dir_cluster == 0);
    const uint32_t bytes_per_entry = sizeof(fat_dir_entry_t);
    uint32_t cur_cluster   = parent_dir_cluster;
    uint32_t last_cluster  = parent_dir_cluster; // Track the last valid cluster before needing allocation
    uint32_t byte_offset   = 0; // Current byte offset within the entire directory chain
    uint32_t cand_offset   = 0; // Starting offset of the current candidate run of free slots
    size_t   free_run      = 0; // Length of the current run of free slots

    // Allocate a buffer for reading directory sectors
    uint8_t *sector_buf = kmalloc(fs->bytes_per_sector);
    if (!sector_buf) {
        FAT_ERROR_LOG("Failed to allocate sector buffer.");
        return FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p (%u bytes).", sector_buf, fs->bytes_per_sector);


    int status = FS_ERR_NO_SPACE; // Assume no space found initially

    // --- Scan Directory Cluster Chain ---
    while (true) {
        // Check for end of cluster chain (only if not scanning fixed root)
        if (cur_cluster >= fs->eoc_marker && !fixed_root) {
             FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu).", (unsigned long)cur_cluster, (unsigned long)fs->eoc_marker);
             break; // Exit loop, will attempt allocation if possible
        }

        uint32_t sector_idx   = byte_offset / fs->bytes_per_sector;
        uint32_t entries_per_sector = fs->bytes_per_sector / bytes_per_entry;

        // Check for end of fixed-size root directory (FAT12/16)
        if (fixed_root && byte_offset >= fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu).",
                          (unsigned long)byte_offset, (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
             break; // Exit loop, cannot extend fixed root
        }

        FAT_DEBUG_LOG("Loop: current_cluster=%lu, current_byte_offset=%lu", (unsigned long)cur_cluster, (unsigned long)byte_offset);

        // Read the current directory sector
        status = read_directory_sector(fs, cur_cluster, sector_idx, sector_buf);
        if (status != FS_SUCCESS) {
            FAT_ERROR_LOG("read_directory_sector failed (err %d) for cluster %lu, sector_idx %lu.",
                           status, (unsigned long)cur_cluster, (unsigned long)sector_idx);
            goto out; // Exit with I/O error
        }
         FAT_DEBUG_LOG("Sector read success (cluster %lu, sector_idx %lu). Processing %lu entries...",
                       (unsigned long)cur_cluster, (unsigned long)sector_idx, (unsigned long)entries_per_sector);

        // Scan entries within the current sector
        for (size_t i = 0; i < entries_per_sector; ++i) {
            fat_dir_entry_t *de = (fat_dir_entry_t *)(sector_buf + i * bytes_per_entry);
            uint8_t tag = de->name[0]; // First byte indicates entry status

            // Calculate absolute offset of this entry within the directory chain
            uint32_t current_entry_abs_offset = byte_offset + (uint32_t)i * bytes_per_entry;
            FAT_DEBUG_LOG("  Entry %lu (abs_offset %lu): Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)i, (unsigned long)current_entry_abs_offset, tag, de->attr);


            if (tag == FAT_DIR_ENTRY_UNUSED || tag == FAT_DIR_ENTRY_DELETED) {
                // Found a free or deleted slot
                if (free_run == 0) {
                    // Start of a new potential run
                    cand_offset = current_entry_abs_offset;
                    FAT_DEBUG_LOG("  Found start of free run at offset %lu", (unsigned long)cand_offset);
                }
                ++free_run;
                FAT_DEBUG_LOG("  Incremented free run to %lu (needed %lu)", (unsigned long)free_run, (unsigned long)needed_slots);

                // Check if we found enough contiguous slots OR hit the end marker
                if (free_run >= needed_slots || tag == FAT_DIR_ENTRY_UNUSED) {
                     FAT_DEBUG_LOG("  Condition met: (free_run %lu >= needed %lu) OR (tag==UNUSED %d)",
                                   (unsigned long)free_run, (unsigned long)needed_slots, (tag == FAT_DIR_ENTRY_UNUSED));
                    *out_slot_cluster = cur_cluster;
                    *out_slot_offset  = cand_offset; // Return the start offset of the suitable run
                    status = FS_SUCCESS;
                    FAT_INFO_LOG("Found suitable slot(s): Cluster=%lu, Offset=%lu (needed %lu, found run %lu)",
                                 (unsigned long)*out_slot_cluster, (unsigned long)*out_slot_offset,
                                 (unsigned long)needed_slots, (unsigned long)free_run);
                    goto out; // Successfully found a spot
                }
            } else {
                // Encountered an in-use entry, reset the free run count
                if (free_run > 0) {
                    FAT_DEBUG_LOG("  Resetting free run count (was %lu)", (unsigned long)free_run);
                }
                free_run = 0;
            }

            // If we just processed the UNUSED marker, we should have exited via 'goto out' above
            if (tag == FAT_DIR_ENTRY_UNUSED) {
                // This part should ideally not be reached if the logic above is correct,
                // but it acts as a safeguard.
                FAT_DEBUG_LOG("  Exiting search loop after hitting UNUSED marker (should have succeeded).");
                status = FS_ERR_NO_SPACE; // Indicate failure if we didn't succeed above
                goto out;
            }
        } // End loop through entries in sector

        // Advance to the next sector's byte offset
        byte_offset += fs->bytes_per_sector;

        // Move to the next cluster if we've finished the current one (and not in fixed root)
        if (!fixed_root && (byte_offset % fs->cluster_size_bytes == 0)) {
            FAT_DEBUG_LOG("End of cluster %lu reached (offset %lu). Finding next...", (unsigned long)cur_cluster, (unsigned long)byte_offset);
            last_cluster = cur_cluster; // Remember the last cluster we successfully processed
            uint32_t next;
            status = fat_get_next_cluster(fs, cur_cluster, &next);
            if (status != FS_SUCCESS) {
                 FAT_ERROR_LOG("Failed to get next cluster after %lu (err %d)", (unsigned long)cur_cluster, status);
                 goto out; // Exit with I/O error
            }
            FAT_DEBUG_LOG("Next cluster in chain: %lu", (unsigned long)next);
            if (next >= fs->eoc_marker) {
                 FAT_DEBUG_LOG("Reached end of chain marker (%lu).", (unsigned long)next);
                 break; // Exit loop, will attempt allocation
            }
            cur_cluster  = next;
            byte_offset  = 0; // Reset byte offset for the new cluster
            free_run = 0; // Reset free run count when moving to a new cluster
             FAT_DEBUG_LOG("Moved to next cluster %lu.", (unsigned long)cur_cluster);
        }
    } // End while(true) loop

    // --- If loop finished without finding enough space ---
    FAT_DEBUG_LOG("Loop finished. Current status: %d (%s)", status, fs_strerror(status));

    // Try to extend the directory by allocating a new cluster (only if not fixed root)
    if (status == FS_ERR_NO_SPACE && !fixed_root) {
        FAT_INFO_LOG("No suitable free slot found in existing clusters. Attempting to extend directory (last cluster: %lu)...", (unsigned long)last_cluster);
        uint32_t new_clu = fat_allocate_cluster(fs, last_cluster); // Allocate and link
        if (new_clu == 0 || new_clu < 2) {
            FAT_ERROR_LOG("Failed to allocate new cluster for directory extension (err %d / clu %lu)", status, (unsigned long)new_clu);
            status = FS_ERR_NO_SPACE; // Allocation failed
            goto out;
        }
        FAT_INFO_LOG("Successfully allocated and linked new cluster %lu for directory.", (unsigned long)new_clu);

        // Zero out the newly allocated cluster
        FAT_DEBUG_LOG("Zeroing out new cluster %lu...", (unsigned long)new_clu);
        memset(sector_buf, 0, fs->bytes_per_sector); // Use existing buffer
        uint32_t lba = fat_cluster_to_lba(fs, new_clu);
        if (lba == 0) {
            FAT_ERROR_LOG("Failed to convert new cluster %lu to LBA!", (unsigned long)new_clu);
            status = FS_ERR_IO;
            goto out_free; // Free the cluster we allocated but couldn't use
        }
        for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s) {
            FAT_DEBUG_LOG("Zeroing sector %lu (LBA %lu) of new cluster %lu", (unsigned long)s, (unsigned long)(lba+s), (unsigned long)new_clu);
            buffer_t *b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba + s);
            if (!b) {
                FAT_ERROR_LOG("Failed to get buffer for LBA %lu during zeroing!", (unsigned long)(lba+s));
                status = FS_ERR_IO;
                goto out_free; // Free the cluster we allocated but couldn't zero
            }
            memcpy(b->data, sector_buf, fs->bytes_per_sector); // Write zeros
            buffer_mark_dirty(b);
            buffer_release(b);
        }
        FAT_DEBUG_LOG("New cluster %lu zeroed successfully.", (unsigned long)new_clu);

        // Success! The free slot starts at the beginning of the new cluster
        *out_slot_cluster = new_clu;
        *out_slot_offset  = 0;
        status = FS_SUCCESS;
        FAT_INFO_LOG("Directory extended. Free slot at start of new cluster %lu (offset 0).", (unsigned long)new_clu);
        goto out; // Return success
    }

out_free:
    // If we allocated a cluster but failed to use/zero it
    if (status != FS_SUCCESS && !fixed_root && cur_cluster != last_cluster && cur_cluster >=2) {
         FAT_WARN_LOG("Error after allocating cluster %lu. Attempting to free it.", (unsigned long)cur_cluster);
         // We only allocated one cluster in this context
         fat_set_cluster_entry(fs, last_cluster, fs->eoc_marker); // Unlink last cluster
         fat_set_cluster_entry(fs, cur_cluster, 0); // Mark the failed allocation as free
    }
out:
    FAT_DEBUG_LOG("Exit: Freeing buffer %p, returning status %d (%s)", sector_buf, status, fs_strerror(status));
    kfree(sector_buf);
    return status;
}