/**
 * @file fat_fs.c
 * @brief Filesystem-level operations implementation for FAT driver.
 *
 * Implements mount, unmount, and FAT table loading/flushing logic.
 */

 #include <kernel/fs/fat/fat_fs.h>     // Our function declarations
 #include <kernel/fs/fat/fat_core.h>   // Core FAT structures and constants
 #include <kernel/fs/fat/fat_utils.h>  // fat_cluster_to_lba (needed for geometry checks?) - maybe not needed here directly
 #include <kernel/drivers/storage/disk.h>       // For reading boot sector, FAT sectors
 #include <kernel/drivers/storage/buffer_cache.h> // Buffer cache for disk I/O
 #include <kernel/memory/kmalloc.h>    // Kernel memory allocation
 #include <kernel/drivers/display/terminal.h>   // Logging
 #include <kernel/sync/spinlock.h>   // Spinlock initialization
 #include <kernel/fs/vfs/fs_errno.h>   // Filesystem error codes
 #include <kernel/lib/string.h>     // memcpy, memset, memcmp
 #include <kernel/lib/assert.h>     // KERNEL_ASSERT
 #include <libc/limits.h> // For SIZE_MAX
 
 
 /* --- Static Helper Prototypes --- */
 
 /**
  * @brief Loads the entire FAT table from disk into memory.
  * @param fs Pointer to the fat_fs_t structure to populate.
  * @return FS_SUCCESS or negative error code.
  */
 static int load_fat_table(fat_fs_t *fs);
 
 /**
  * @brief Flushes the in-memory FAT table back to disk via buffer cache if modified.
  * @param fs Pointer to the fat_fs_t structure containing the FAT table.
  * @return FS_SUCCESS or negative error code.
  */
 static int flush_fat_table(fat_fs_t *fs);
 
 
 /* --- VFS Mount/Unmount Implementations --- */
 
 /**
  * @brief Mounts a FAT filesystem on a specified block device.
  */
 void *fat_mount_internal(const char *device_name)
 {
     terminal_printf("[FAT Mount] Attempting mount for device '%s'...\n", device_name ? device_name : "<NULL>");
     if (!device_name || device_name[0] == '\0') {
         terminal_write("[FAT Mount] Error: Invalid device name provided.\n");
         return NULL; // Invalid parameter
     }
 
     fat_fs_t *fs = NULL;
     buffer_t *bs_buf = NULL; // Buffer for boot sector
     int result = FS_SUCCESS;
 
     // 1. Allocate filesystem structure
     fs = kmalloc(sizeof(fat_fs_t));
     if (!fs) {
         terminal_write("[FAT Mount] Error: Failed to allocate memory for fat_fs_t.\n");
         result = FS_ERR_OUT_OF_MEMORY;
         goto mount_fail;
     }
     memset(fs, 0, sizeof(*fs));
     spinlock_init(&fs->lock); // Initialize the lock early
     fs->fat_table = NULL; // Ensure fat_table is NULL initially for cleanup logic
     fs->fat_dirty = false; // Initialize dirty flag
 
     // 2. Read Boot Sector (LBA 0) using Buffer Cache
     //    Buffer cache implicitly looks up the disk_t based on device_name.
     bs_buf = buffer_get(device_name, 0);
     if (!bs_buf) {
         terminal_printf("[FAT Mount] Error: Failed to read boot sector (LBA 0) for device '%s' via buffer cache.\n", device_name);
         result = FS_ERR_IO;
         goto mount_fail;
     }
     // Ensure the buffer cache found the underlying disk structure
     if (!bs_buf->disk) {
          terminal_printf("[FAT Mount] Error: Buffer cache lookup failed for device '%s' (disk_t is NULL).\n", device_name);
          result = FS_ERR_INTERNAL; // Or perhaps FS_ERR_INVALID_PARAM if device isn't registered
          goto mount_fail;
     }
     // Store the pointer to the disk structure
     fs->disk_ptr = bs_buf->disk;
 
     // 3. Parse and Validate Boot Sector / BPB
     // Copy to a local struct to avoid alignment issues and release buffer early.
     fat_boot_sector_t bpb;
     memcpy(&bpb, bs_buf->data, sizeof(fat_boot_sector_t)); // Assuming bs_buf->data is valid & sector-sized
 
     // Check boot signature (0xAA55 at offset 510)
     if (((uint8_t*)bs_buf->data)[510] != 0x55 || ((uint8_t*)bs_buf->data)[511] != 0xAA) {
         terminal_printf("[FAT Mount] Error: Invalid boot sector signature (0xAA55 missing) on device '%s'.\n", device_name);
         result = FS_ERR_INVALID_FORMAT;
         goto mount_fail;
     }
     buffer_release(bs_buf); // Release boot sector buffer now
     bs_buf = NULL;
 
     // 4. Extract and Validate Geometry from BPB
     fs->bytes_per_sector = bpb.bytes_per_sector;
     fs->sectors_per_cluster = bpb.sectors_per_cluster;
     // Sanity checks: Sector size must be power-of-2 >= 512. Sectors/cluster must be power-of-2 > 0.
     if (fs->bytes_per_sector < 512 || fs->bytes_per_sector > 4096 || (fs->bytes_per_sector & (fs->bytes_per_sector - 1)) != 0 ||
         fs->sectors_per_cluster == 0 || fs->sectors_per_cluster > 128 || (fs->sectors_per_cluster & (fs->sectors_per_cluster - 1)) != 0)
     {
         terminal_printf("[FAT Mount] Error: Invalid geometry on device '%s' (BPB BytesPerSector=%u, SectorsPerCluster=%u).\n",
                         device_name, fs->bytes_per_sector, fs->sectors_per_cluster);
         result = FS_ERR_INVALID_FORMAT;
         goto mount_fail;
     }
     fs->cluster_size_bytes = (uint32_t)fs->bytes_per_sector * fs->sectors_per_cluster; // Check for overflow? Unlikely.
 
     // Determine Total Sectors and FAT Size (handle short vs long fields)
     fs->total_sectors = (bpb.total_sectors_short != 0) ? bpb.total_sectors_short : bpb.total_sectors_long;
     fs->fat_size_sectors = (bpb.fat_size_16 != 0) ? bpb.fat_size_16 : bpb.fat_size_32;
     fs->num_fats = bpb.num_fats;
 
     // Validate critical BPB values
     if (fs->total_sectors == 0 || fs->fat_size_sectors == 0 || fs->num_fats == 0 || bpb.reserved_sector_count == 0) {
         terminal_printf("[FAT Mount] Error: Invalid BPB values on device '%s' (TotalSect=%u, FATSize=%u, NumFATs=%u, Resvd=%u).\n",
                         device_name, fs->total_sectors, fs->fat_size_sectors, fs->num_fats, bpb.reserved_sector_count);
         result = FS_ERR_INVALID_FORMAT;
         goto mount_fail;
     }
     fs->fat_start_lba = bpb.reserved_sector_count;
 
     // Calculate Root Directory info (only relevant for FAT12/16)
     // Note: root_entry_count is 0 for FAT32.
     uint32_t root_dir_bytes = (uint32_t)bpb.root_entry_count * sizeof(fat_dir_entry_t);
     fs->root_dir_sectors = (root_dir_bytes + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
     fs->root_dir_start_lba = fs->fat_start_lba + ((uint32_t)fs->num_fats * fs->fat_size_sectors);
 
     // Calculate Data Area start and Cluster Count
     fs->first_data_sector = fs->root_dir_start_lba + fs->root_dir_sectors; // Sector after root dir (or FATs for FAT32)
     if (fs->first_data_sector >= fs->total_sectors) {
         terminal_printf("[FAT Mount] Error: Calculated data sector start (%u) beyond total sectors (%u).\n",
                          fs->first_data_sector, fs->total_sectors);
         result = FS_ERR_INVALID_FORMAT;
         goto mount_fail;
     }
     uint32_t data_sectors = fs->total_sectors - fs->first_data_sector;
     if (fs->sectors_per_cluster == 0) { // Prevent division by zero
          terminal_printf("[FAT Mount] Error: Sectors per cluster is zero.\n");
          result = FS_ERR_INVALID_FORMAT;
          goto mount_fail;
     }
     fs->total_data_clusters = data_sectors / fs->sectors_per_cluster;
 
     // 5. Determine FAT Type (based on cluster count)
     // These thresholds are standard.
     if (fs->total_data_clusters < 4085) {
         fs->type = FAT_TYPE_FAT12;
         fs->root_cluster = 0; // Root dir is fixed area, not cluster based
         fs->eoc_marker = 0x0FF8; // Standard EOC range 0xFF8-0xFFF
         // Correct data area start for FAT12 (after fixed root dir)
         fs->first_data_sector = fs->root_dir_start_lba + fs->root_dir_sectors;
         terminal_write("[FAT Mount] Detected FAT12.\n");
          // FAT12 support might require special handling in get/set cluster entry funcs. Add warning?
         terminal_write("[FAT Mount] Warning: FAT12 support might be incomplete/untested.\n");
     } else if (fs->total_data_clusters < 65525) {
         fs->type = FAT_TYPE_FAT16;
         fs->root_cluster = 0; // Root dir is fixed area
         fs->eoc_marker = 0xFFF8; // Standard EOC range 0xFFF8-0xFFFF
         // Correct data area start for FAT16 (after fixed root dir)
         fs->first_data_sector = fs->root_dir_start_lba + fs->root_dir_sectors;
         terminal_write("[FAT Mount] Detected FAT16.\n");
     } else {
         fs->type = FAT_TYPE_FAT32;
         fs->root_cluster = bpb.root_cluster; // Root dir is a cluster chain
         fs->eoc_marker = 0x0FFFFFF8; // Standard EOC range 0x0FFFFFF8-0x0FFFFFFF
         // For FAT32, the data area starts immediately after the FATs (root_entry_count is 0)
         fs->first_data_sector = fs->fat_start_lba + ((uint32_t)fs->num_fats * fs->fat_size_sectors);
         // Recalculate data sectors and cluster count based on this start for consistency check
         // data_sectors = fs->total_sectors - fs->first_data_sector;
         // fs->total_data_clusters = data_sectors / fs->sectors_per_cluster;
         if (fs->root_cluster < 2) { // Root cluster must be valid
              terminal_printf("[FAT Mount] Error: Invalid root cluster value (%u) for FAT32.\n", fs->root_cluster);
              result = FS_ERR_INVALID_FORMAT;
              goto mount_fail;
         }
         terminal_write("[FAT Mount] Detected FAT32.\n");
     }
      // Final check on data sector calculation
      if (fs->first_data_sector >= fs->total_sectors) {
         terminal_printf("[FAT Mount] Error: Final data sector start (%u) beyond total sectors (%u).\n",
                          fs->first_data_sector, fs->total_sectors);
         result = FS_ERR_INVALID_FORMAT;
         goto mount_fail;
      }
 
     // 6. Load FAT Table into Memory
     result = load_fat_table(fs);
     if (result != FS_SUCCESS) {
         terminal_printf("[FAT Mount] Error: Failed to load FAT table for device '%s' (code %d).\n", device_name, result);
         // load_fat_table frees fs->fat_table on failure
         goto mount_fail; // fs itself will be freed below
     }
 
     // --- Mount Successful ---
     terminal_printf("[FAT Mount] Mount successful for device '%s'. Type: FAT%d\n",
                      device_name, (fs->type == FAT_TYPE_FAT12) ? 12 : (fs->type == FAT_TYPE_FAT16 ? 16 : 32));
     return fs; // Return the allocated and initialized context
 
 
 mount_fail:
     // Cleanup logic for mount failure
     terminal_printf("[FAT Mount] Mount failed for device '%s' (Error code: %d).\n", device_name, result);
     if (bs_buf) {
         buffer_release(bs_buf); // Release buffer if held
     }
     if (fs) {
         // If FAT table was allocated by load_fat_table but mount failed later
         if (fs->fat_table) {
             kfree(fs->fat_table);
         }
         kfree(fs); // Free the main fs structure
     }
     // fs_set_errno(result); // Set thread-local errno maybe
     return NULL; // Indicate failure
 }
 
 /**
  * @brief Unmounts a FAT filesystem instance.
  */
 int fat_unmount_internal(void *fs_context)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs) {
         terminal_printf("[FAT Unmount] Error: Invalid NULL context provided.\n");
         return FS_ERR_INVALID_PARAM;
     }
      // Use disk name from stored pointer if available, otherwise context address for logging
     const char *dev_name = (fs->disk_ptr && fs->disk_ptr->blk_dev.device_name) ?
                             fs->disk_ptr->blk_dev.device_name : "(unknown device)";
     terminal_printf("[FAT Unmount] Unmounting FAT filesystem for %s (context @ 0x%p)...\n", dev_name, fs);
 
     // Acquire lock to ensure exclusive access during unmount
     // This prevents races if another thread tries accessing the FS during unmount.
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
 
     int result = FS_SUCCESS;
 
     // 1. Flush the in-memory FAT table if it exists and is dirty
     if (fs->fat_table) {
         result = flush_fat_table(fs); // flush_fat_table handles fs->fat_dirty check internally
         if (result != FS_SUCCESS) {
              terminal_printf("[FAT Unmount] Warning: Failed to flush FAT table for %s (err %d). Continuing unmount.\n",
                              dev_name, result);
             // Log the error but continue cleanup. Data loss might have occurred.
         }
         // Free the FAT table memory regardless of flush success
         kfree(fs->fat_table);
         fs->fat_table = NULL;
     }
 
     // 2. Optionally sync the entire buffer cache for the device. Good practice.
     //    This ensures directory entries, data blocks etc. are written out.
     if (fs->disk_ptr && fs->disk_ptr->blk_dev.device_name) {
         // Corrected: Replace missing buffer_cache_sync_device with buffer_cache_sync
         buffer_cache_sync(); // Sync all dirty buffers across all devices
         terminal_printf("[FAT Unmount] Called buffer_cache_sync().\n");
     }
 
     // 3. Release the lock before freeing the context structure itself
     spinlock_release_irqrestore(&fs->lock, irq_flags);
 
     // 4. Free the filesystem context structure
     kfree(fs);
 
     terminal_printf("[FAT Unmount] Unmount complete for %s.\n", dev_name);
     return result; // Return status of flush operation (or SUCCESS if no flush needed/done)
 }
 
 
 /* --- Static Helper Implementations --- */
 
 /**
  * @brief Loads the entire FAT table from disk into memory.
  * (Implementation based on original fat.c load_fat_table)
  */
 static int load_fat_table(fat_fs_t *fs)
 {
     // Corrected: Added message strings to KERNEL_ASSERTs
     KERNEL_ASSERT(fs != NULL && fs->disk_ptr != NULL && fs->disk_ptr->blk_dev.device_name != NULL,
                   "FS context, disk pointer, and device name must be valid in load_fat_table");
     KERNEL_ASSERT(fs->fat_table == NULL, "FAT table already loaded, should not call load_fat_table again");
     KERNEL_ASSERT(fs->fat_size_sectors > 0 && fs->bytes_per_sector > 0,
                   "FAT size in sectors and bytes per sector must be positive");
 
     terminal_write("[FAT Load FAT] Loading FAT table...\n");
 
     size_t bytes_per_sector_sz = fs->bytes_per_sector;
     size_t fat_size_sectors_sz = fs->fat_size_sectors;
 
     // Check for potential overflow before multiplication
     if (bytes_per_sector_sz > 0 && fat_size_sectors_sz > SIZE_MAX / bytes_per_sector_sz) {
         terminal_write("[FAT Load FAT] Error: FAT table size calculation overflows size_t.\n");
         return FS_ERR_OVERFLOW;
     }
     fs->fat_table_size_bytes = fat_size_sectors_sz * bytes_per_sector_sz;
 
     terminal_printf("[FAT Load FAT] Calculated FAT table size: %u bytes (%u sectors).\n",
                      (unsigned int)fs->fat_table_size_bytes, fs->fat_size_sectors);
 
     // Allocate memory for the FAT table
     fs->fat_table = kmalloc(fs->fat_table_size_bytes);
     if (!fs->fat_table) {
         terminal_printf("[FAT Load FAT] Error: Failed to allocate %u bytes for FAT table.\n",
                          (unsigned int)fs->fat_table_size_bytes);
         fs->fat_table_size_bytes = 0; // Reset size
         return FS_ERR_OUT_OF_MEMORY;
     }
 
     // Read FAT sectors using buffer cache
     terminal_printf("[FAT Load FAT] Reading %u FAT sectors starting from LBA %u...\n",
                      fs->fat_size_sectors, fs->fat_start_lba);
     uint8_t* current_fat_ptr = (uint8_t*)fs->fat_table;
 
     for (uint32_t sector_index = 0; sector_index < fs->fat_size_sectors; sector_index++) {
         uint32_t lba = fs->fat_start_lba + sector_index;
         buffer_t* sector_buf = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!sector_buf) {
             terminal_printf("[FAT Load FAT] Error: Failed to get buffer for FAT sector %u (LBA %u).\n", sector_index, lba);
             kfree(fs->fat_table); // Clean up allocation
             fs->fat_table = NULL;
             fs->fat_table_size_bytes = 0;
             return FS_ERR_IO;
         }
 
         // Copy data from buffer cache to our allocated FAT table buffer
         memcpy(current_fat_ptr, sector_buf->data, fs->bytes_per_sector);
 
         // Release the buffer cache block
         buffer_release(sector_buf);
 
         // Advance pointer in our FAT table buffer
         current_fat_ptr += fs->bytes_per_sector;
     }
 
     fs->fat_dirty = false; // Mark FAT as clean initially after loading
     terminal_write("[FAT Load FAT] FAT table loaded successfully.\n");
     return FS_SUCCESS;
 }
 
 /**
  * @brief Flushes the in-memory FAT table back to disk via buffer cache if modified.
  * (Implementation based on original fat.c flush_fat_table_v2 with comparison)
  */
 static int flush_fat_table(fat_fs_t *fs)
 {
     // Corrected: Added message string to KERNEL_ASSERT
     KERNEL_ASSERT(fs != NULL, "FS context cannot be NULL in flush_fat_table");
     // Assumes caller holds lock if concurrent modification is possible
 
     if (!fs->fat_table || !fs->fat_dirty) {
         // Nothing to flush (not loaded, or not modified)
         return FS_SUCCESS;
     }
     if (fs->fat_size_sectors == 0 || fs->bytes_per_sector == 0 || !fs->disk_ptr || !fs->disk_ptr->blk_dev.device_name) {
         terminal_printf("[FAT Flush FAT] Error: Invalid FS state for flushing (size=%u, bps=%u, disk=%p).\n",
                          fs->fat_size_sectors, fs->bytes_per_sector, fs->disk_ptr);
         return FS_ERR_INTERNAL; // Indicates FS struct wasn't properly initialized
     }
 
     terminal_printf("[FAT Flush FAT] Flushing %u modified FAT sectors via buffer cache...\n", fs->fat_size_sectors);
     int sectors_written = 0;
     int errors_encountered = 0;
     const uint8_t *current_fat_ptr = (const uint8_t *)fs->fat_table;
 
     // Iterate through each sector of the FAT table
     for (uint32_t i = 0; i < fs->fat_size_sectors; i++) {
         uint32_t target_lba = fs->fat_start_lba + i;
         const uint8_t *fat_sector_in_memory = current_fat_ptr + (i * fs->bytes_per_sector);
 
         // Get the corresponding buffer from the cache
         // This might read from disk if not present, but that's okay.
         buffer_t *cached_buf = buffer_get(fs->disk_ptr->blk_dev.device_name, target_lba);
         if (!cached_buf) {
             terminal_printf("[FAT Flush FAT] Error: Failed to get buffer for LBA %u (FAT sector %u).\n", target_lba, i);
             errors_encountered++;
             continue; // Try to flush subsequent sectors
         }
 
         // --- Optimization: Compare in-memory FAT sector with cached buffer ---
         // Only write if they differ. memcmp returns 0 if identical.
         if (memcmp(cached_buf->data, fat_sector_in_memory, fs->bytes_per_sector) != 0) {
             // Contents differ - update the cache buffer and mark it dirty
             memcpy(cached_buf->data, fat_sector_in_memory, fs->bytes_per_sector);
             buffer_mark_dirty(cached_buf);
             sectors_written++;
         }
 
         // Release the buffer (decrements ref count)
         buffer_release(cached_buf);
     }
 
     // Only clear the dirty flag if no errors occurred during the flush attempt
     if (errors_encountered == 0) {
         fs->fat_dirty = false;
         terminal_printf("[FAT Flush FAT] Flush complete. %d sectors written.\n", sectors_written);
         return FS_SUCCESS;
     } else {
         terminal_printf("[FAT Flush FAT] Flush completed with %d errors. %d sectors written. FAT remains marked dirty.\n",
                          errors_encountered, sectors_written);
         // Do NOT clear fs->fat_dirty - indicates flush may be incomplete
         return FS_ERR_IO;
     }
 }