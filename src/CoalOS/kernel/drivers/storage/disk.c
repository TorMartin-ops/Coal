/**
 * @file disk.c
 * @brief Logical Disk Layer (Handles MBR Parsing)
 * @version 1.1 - Added KBC debug prints, fixed build errors.
 */

 #include <kernel/drivers/storage/disk.h>
 #include <kernel/drivers/storage/block_device.h>   // Underlying device operations
 #include <kernel/drivers/display/terminal.h>       // Logging
 #include <kernel/fs/vfs/fs_errno.h>       // Error codes
 #include <kernel/memory/kmalloc.h>        // For temporary buffer allocation
 #include <kernel/core/types.h>          // Standard types
 #include <kernel/lib/assert.h>         // For KERNEL_ASSERT
 #include <kernel/lib/port_io.h>        // <<< ADDED for inb
 #include <kernel/drivers/input/keyboard_hw.h>    // <<< ADDED for KBC_STATUS_PORT
 
 #include <kernel/lib/string.h>         // For memset, memcpy
 
 // --- MBR Structure Definitions ---
 // Standard MBR Partition Entry (16 bytes)
 typedef struct __attribute__((packed)) {
     uint8_t  boot_indicator; // 0x80 = bootable, 0x00 = non-bootable
     uint8_t  start_head;
     uint16_t start_sector_cylinder; // Encoded start sector/cylinder (CHS) - Ignore for LBA
     uint8_t  partition_type;
     uint8_t  end_head;
     uint16_t end_sector_cylinder;   // Encoded end sector/cylinder (CHS) - Ignore for LBA
     uint32_t start_lba;       // Starting LBA sector of the partition
     uint32_t total_sectors;   // Number of sectors in the partition
 } mbr_partition_entry_t;
 
 // Standard MBR Structure (relevant parts)
 typedef struct __attribute__((packed)) {
     uint8_t               bootstrap_code[446]; // Boot code area
     mbr_partition_entry_t partitions[4];       // Partition table (4 entries)
     uint16_t              signature;           // MBR signature (0xAA55)
 } master_boot_record_t;
 
 
 // --- Static Helper Prototypes ---
 static int parse_mbr(disk_t *disk);
 
 
 /**
  * @brief Initializes a disk structure, probing the underlying block device and parsing partitions.
  * Initializes the block device, reads the MBR, parses partitions, and marks the disk as ready.
  * @param disk Pointer to the disk_t structure to initialize.
  * @param device_name The kernel name for the block device (e.g., "hda", "hdb").
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int disk_init(disk_t *disk, const char *device_name) {
     if (!disk || !device_name) {
         terminal_printf("[Disk] disk_init: Error - Invalid parameters (disk=%p, name=%s).\n", disk, device_name ? device_name : "NULL");
         return FS_ERR_INVALID_PARAM; // Use enum value directly
     }
 
     // Zero initialize the entire disk structure
     memset(disk, 0, sizeof(disk_t));
 
     // 1. Initialize the underlying block device
     terminal_printf("[Disk] Initializing block device '%s'...\n", device_name);
 
     // --- Debug Print BEFORE block_device_init ---
     terminal_printf("[Disk Debug] KBC Status before block_device_init: 0x%x\n", inb(KBC_STATUS_PORT));
     // --- End Debug Print ---
 
     // <<< FIX: Declare ret *before* the call >>>
     int ret = block_device_init(device_name, &disk->blk_dev);
 
     // --- Debug Print AFTER block_device_init ---
     terminal_printf("[Disk Debug] KBC Status after block_device_init: 0x%x\n", inb(KBC_STATUS_PORT));
     // --- End Debug Print ---
 
 
     if (ret != FS_SUCCESS) {
         terminal_printf("[Disk] disk_init: Error - Underlying block device init failed for '%s' (code %d).\n",
                         device_name, ret);
         disk->initialized = false; // Ensure it's marked as not initialized
         return ret; // Propagate block device error
     }
     // <<< FIX: Use %lu for uint32_t lower part (avoid (uint32_t)(%llu0xFFFFFFFF) with %lu) >>>
     terminal_printf("[Disk] Block device '%s' initialized. Total Sectors: %lu\n",
                     disk->blk_dev.device_name, (uint32_t)(disk->blk_dev.total_sectors & 0xFFFFFFFF));
 
     // *** FIX APPLIED HERE ***
     // 2. Mark disk structure as initialized *before* trying to read from it
     disk->initialized = true;
 
     // 3. Attempt to parse the MBR partition table
     ret = parse_mbr(disk); // Now safe to call as disk->initialized is true
     if (ret != FS_SUCCESS) {
         // Don't treat MBR parsing failure as a fatal disk init error.
         terminal_printf("[Disk] disk_init: Warning - Failed to parse MBR on '%s' (code %d). Disk usable for raw access.\n",
                         disk->blk_dev.device_name, ret);
         disk->has_mbr = false;
     } else {
         disk->has_mbr = true;
         terminal_printf("[Disk] disk_init: Successfully parsed MBR on '%s'.\n", disk->blk_dev.device_name);
     }
 
     // 4. Final completion message
     // disk->initialized = true; // <-- REMOVED FROM HERE
     terminal_printf("[Disk] Logical disk '%s' initialization complete.\n", disk->blk_dev.device_name);
     return FS_SUCCESS;
 }
 
 /**
  * @brief Reads sectors directly from the underlying block device (ignores partitions).
  * Provides raw access to the disk, bypassing partition logic.
  * @param disk Pointer to the initialized disk_t structure.
  * @param lba The starting Logical Block Address (absolute on disk).
  * @param buffer Pointer to the buffer where data will be stored.
  * @param count Number of sectors to read.
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int disk_read_raw_sectors(disk_t *disk, uint64_t lba, void *buffer, size_t count) {
     if (!disk || !disk->initialized || !buffer || count == 0) {
         // <<< FIX: Use %lu for size_t >>>
         terminal_printf("[Disk] read_raw: Error - Invalid parameters (disk=%p, init=%d, buf=%p, count=%lu).\n",
                         disk, disk ? disk->initialized : 0, buffer, (unsigned long)count);
         return FS_ERR_INVALID_PARAM;
     }
 
     // Bounds check against the whole disk size
     if (lba >= disk->blk_dev.total_sectors || count > disk->blk_dev.total_sectors - lba) {
          // <<< FIX: Use %lu for size_t, %lu for uint32_t part of uint64_t >>>
          terminal_printf("[Disk] read_raw: Error - Read out of bounds (LBA=%lu, Count=%lu, Total=%lu).\n",
                          (uint32_t)(lba & 0xFFFFFFFF), (unsigned long)count, (uint32_t)(disk->blk_dev.total_sectors & 0xFFFFFFFF));
          return FS_ERR_OUT_OF_BOUNDS;
     }
 
     // Delegate directly to the block device function
     int ret = block_device_read(&disk->blk_dev, lba, buffer, count);
     if (ret != FS_SUCCESS) {
         // <<< FIX: Use %lu for size_t, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
         terminal_printf("[Disk] read_raw: Block device read failed for %lu sectors at LBA (uint32_t)(%llu0xFFFFFFFF) with %lu.\n", (unsigned long)count, lba);
     }
     return ret;
 }
 
 /**
  * @brief Writes sectors directly to the underlying block device (ignores partitions).
  * Provides raw access to the disk, bypassing partition logic.
  * @param disk Pointer to the initialized disk_t structure.
  * @param lba The starting Logical Block Address (absolute on disk).
  * @param buffer Pointer to the buffer containing data to write.
  * @param count Number of sectors to write.
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int disk_write_raw_sectors(disk_t *disk, uint64_t lba, const void *buffer, size_t count) {
     if (!disk || !disk->initialized || !buffer || count == 0) {
         // <<< FIX: Use %lu for size_t >>>
         terminal_printf("[Disk] write_raw: Error - Invalid parameters (disk=%p, init=%d, buf=%p, count=%lu).\n",
                         disk, disk ? disk->initialized : 0, buffer, (unsigned long)count);
         return FS_ERR_INVALID_PARAM;
     }
 
     // Bounds check against the whole disk size
     if (lba >= disk->blk_dev.total_sectors || count > disk->blk_dev.total_sectors - lba) {
          // <<< FIX: Use %lu for size_t, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
          terminal_printf("[Disk] write_raw: Error - Write out of bounds (LBA=(uint32_t)(%llu0xFFFFFFFF) with %lu, Count=%lu, Total=(uint32_t)(%llu0xFFFFFFFF) with %lu).\n",
                          lba, (unsigned long)count, disk->blk_dev.total_sectors);
          return FS_ERR_OUT_OF_BOUNDS;
     }
 
     // Delegate directly to the block device function
     int ret = block_device_write(&disk->blk_dev, lba, buffer, count);
     if (ret != FS_SUCCESS) {
         // <<< FIX: Use %lu for size_t, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
         terminal_printf("[Disk] write_raw: Block device write failed for %lu sectors at LBA (uint32_t)(%llu0xFFFFFFFF) with %lu.\n", (unsigned long)count, lba);
     }
     return ret;
 }
 
 
 /**
  * @brief Reads sectors from a specific partition.
  * Translates the partition-relative LBA to an absolute disk LBA and performs bounds checks.
  * @param partition Pointer to the initialized partition_t structure.
  * @param lba The starting Logical Block Address *relative to the start of the partition*.
  * @param buffer Pointer to the buffer where data will be stored.
  * @param count Number of sectors to read.
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int partition_read_sectors(partition_t *partition, uint64_t lba, void *buffer, size_t count) {
      if (!partition || !partition->is_valid || !partition->parent_disk || !buffer || count == 0) {
          // <<< FIX: Use %lu for size_t >>>
          terminal_printf("[Disk] part_read: Error - Invalid parameters (part=%p, valid=%d, disk=%p, buf=%p, count=%lu).\n",
                          partition, partition ? partition->is_valid : 0, partition ? partition->parent_disk : NULL, buffer, (unsigned long)count);
          return FS_ERR_INVALID_PARAM;
      }
 
      // Bounds check against the partition size
      if (lba >= partition->total_sectors || count > partition->total_sectors - lba) {
          // <<< FIX: Use %lu for size_t, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
          terminal_printf("[Disk] part_read: Error - Read out of partition bounds (Part LBA=(uint32_t)(%llu0xFFFFFFFF) with %lu, Count=%lu, Part Size=(uint32_t)(%llu0xFFFFFFFF) with %lu).\n",
                          lba, (unsigned long)count, partition->total_sectors);
          return FS_ERR_OUT_OF_BOUNDS;
      }
 
      // Translate partition LBA to absolute disk LBA
      uint64_t absolute_lba = partition->start_lba + lba;
 
      // Delegate to the raw disk read function
      return disk_read_raw_sectors(partition->parent_disk, absolute_lba, buffer, count);
 }
 
 /**
  * @brief Writes sectors to a specific partition.
  * Translates the partition-relative LBA to an absolute disk LBA and performs bounds checks.
  * @param partition Pointer to the initialized partition_t structure.
  * @param lba The starting Logical Block Address *relative to the start of the partition*.
  * @param buffer Pointer to the buffer containing data to write.
  * @param count Number of sectors to write.
  * @return FS_SUCCESS on success, negative error code on failure.
  */
 int partition_write_sectors(partition_t *partition, uint64_t lba, const void *buffer, size_t count) {
       if (!partition || !partition->is_valid || !partition->parent_disk || !buffer || count == 0) {
           // <<< FIX: Use %lu for size_t >>>
           terminal_printf("[Disk] part_write: Error - Invalid parameters (part=%p, valid=%d, disk=%p, buf=%p, count=%lu).\n",
                           partition, partition ? partition->is_valid : 0, partition ? partition->parent_disk : NULL, buffer, (unsigned long)count);
           return FS_ERR_INVALID_PARAM;
       }
 
       // Bounds check against the partition size
       if (lba >= partition->total_sectors || count > partition->total_sectors - lba) {
           // <<< FIX: Use %lu for size_t, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
           terminal_printf("[Disk] part_write: Error - Write out of partition bounds (Part LBA=(uint32_t)(%llu0xFFFFFFFF) with %lu, Count=%lu, Part Size=(uint32_t)(%llu0xFFFFFFFF) with %lu).\n",
                           lba, (unsigned long)count, partition->total_sectors);
           return FS_ERR_OUT_OF_BOUNDS;
       }
 
       // Translate partition LBA to absolute disk LBA
       uint64_t absolute_lba = partition->start_lba + lba;
 
       // Delegate to the raw disk write function
       return disk_write_raw_sectors(partition->parent_disk, absolute_lba, buffer, count);
 }
 
 /**
  * @brief Gets a pointer to a partition structure by its index.
  * @param disk Pointer to the initialized disk_t structure.
  * @param index The partition index (0-3 for MBR primary).
  * @return Pointer to the partition_t structure if valid, or NULL otherwise.
  */
 partition_t* disk_get_partition(disk_t *disk, uint8_t index) {
     if (!disk || !disk->initialized || index >= MAX_PARTITIONS_PER_DISK) {
         return NULL;
     }
     // Return pointer only if the partition was marked as valid during parsing
     return disk->partitions[index].is_valid ? &disk->partitions[index] : NULL;
 }
 
 /**
  * @brief Gets the total number of sectors for the entire disk.
  * @param disk Pointer to the initialized disk_t structure.
  * @return Total sectors, or 0 if disk is invalid/uninitialized.
  */
 uint64_t disk_get_total_sectors(disk_t *disk) {
       if (!disk || !disk->initialized) {
           return 0;
       }
       return disk->blk_dev.total_sectors;
 }
 
 
 // --- Static Helper Implementations ---
 
 /**
  * @brief Reads and parses the Master Boot Record (MBR) of a disk.
  * Populates the partition information within the disk_t structure.
  * @param disk Pointer to the disk_t structure (block device must be initialized, and disk->initialized must be true).
  * @return FS_SUCCESS on success (valid MBR found and parsed), negative error code otherwise.
  */
 static int parse_mbr(disk_t *disk) {
     // This assertion should now pass because disk_init sets disk->initialized before calling us.
     KERNEL_ASSERT(disk != NULL && disk->initialized, "Disk pointer must be non-NULL and initialized for MBR parsing");
 
     // Allocate a temporary buffer for the MBR sector
     uint32_t sector_size = disk->blk_dev.sector_size;
     if (sector_size < 512) {
          // <<< FIX: Use %lu for uint32_t >>>
          terminal_printf("[Disk MBR] Error: Disk sector size %lu is less than required 512 bytes.\n", (unsigned long)sector_size);
          return FS_ERR_INVALID_FORMAT;
     }
     uint8_t *mbr_buffer = kmalloc(sector_size);
     if (!mbr_buffer) {
         terminal_printf("[Disk MBR] Error: Failed to allocate buffer for MBR read.\n");
         return FS_ERR_OUT_OF_MEMORY;
     }
 
     // Read the first sector (LBA 0)
     terminal_printf("[Disk MBR] Reading MBR sector (LBA 0) from '%s'...\n", disk->blk_dev.device_name);
     // Use disk_read_raw_sectors which performs necessary checks (incl. disk->initialized)
     int ret = disk_read_raw_sectors(disk, 0, mbr_buffer, 1);
     if (ret != FS_SUCCESS) {
         terminal_printf("[Disk MBR] Error: Failed to read MBR sector (LBA 0) from '%s' (code %d).\n",
                         disk->blk_dev.device_name, ret);
         kfree(mbr_buffer);
         return ret; // Propagate read error
     }
 
     // Check MBR signature (0xAA55 at offset 510)
     master_boot_record_t *mbr = (master_boot_record_t *)mbr_buffer;
     if (mbr->signature != 0xAA55) {
         terminal_printf("[Disk MBR] Warning: Invalid MBR signature (0x%04X) found on '%s'. No partitions parsed.\n",
                         mbr->signature, disk->blk_dev.device_name);
         kfree(mbr_buffer);
         return FS_ERR_INVALID_FORMAT;
     }
 
     terminal_printf("[Disk MBR] Valid MBR signature found. Parsing partitions...\n");
 
     // Parse the 4 primary partition entries
     for (int i = 0; i < MAX_PARTITIONS_PER_DISK; i++) {
         mbr_partition_entry_t *entry = &mbr->partitions[i];
         partition_t *part = &disk->partitions[i];
 
         part->partition_index = i;
         part->parent_disk = disk; // Link back to parent
 
         if (entry->partition_type != 0 && entry->total_sectors != 0) {
             part->is_valid = true;
             part->type = entry->partition_type;
             // MBR uses 32-bit fields for LBA and Size
             part->start_lba = (uint64_t)entry->start_lba;
             part->total_sectors = (uint64_t)entry->total_sectors;
 
             // Basic sanity check against whole disk size
             if (part->start_lba >= disk->blk_dev.total_sectors ||
                 part->total_sectors > disk->blk_dev.total_sectors - part->start_lba)
             {
                  // <<< FIX: Use %d for int i, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
                  terminal_printf("[Disk MBR] Warning: Partition %d on '%s' seems invalid (Start=%lu, Size=%lu, DiskSize=%lu). Marking invalid.\n",
                                  i, disk->blk_dev.device_name, 
                                  (uint32_t)(part->start_lba & 0xFFFFFFFF), 
                                  (uint32_t)(part->total_sectors & 0xFFFFFFFF), 
                                  (uint32_t)(disk->blk_dev.total_sectors & 0xFFFFFFFF));
                  part->is_valid = false;
             } else {
                  // <<< FIX: Use %d for int i, (uint32_t)(%llu0xFFFFFFFF) with %lu for uint64_t >>>
                  terminal_printf("   [+] Partition %d: Type=0x%02X, StartLBA=%lu, Size=%lu sectors\n",
                                  i, part->type, 
                                  (uint32_t)(part->start_lba & 0xFFFFFFFF),
                                  (uint32_t)(part->total_sectors & 0xFFFFFFFF));
             }
         } else {
             part->is_valid = false;
             part->type = 0;
             part->start_lba = 0;
             part->total_sectors = 0;
         }
     }
 
     kfree(mbr_buffer);
     return FS_SUCCESS; // Successfully parsed
 }