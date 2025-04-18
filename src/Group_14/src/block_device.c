// === START: Improved block_device.c (v5 - ASSERTs Removed) ===
/**
 * @file block_device.c
 * @brief ATA PIO Block Device Driver
 *
 * Provides basic initialization and read/write access to ATA devices
 * using Programmed I/O (PIO) mode. Supports LBA28/LBA48 and MULTIPLE mode.
 * Aims for robustness, clarity, and adherence to common ATA practices.
 */

 #include "block_device.h"
 #include "port_io.h"      // For inb, outb, inw, outw
 #include "terminal.h"     // For terminal_printf/write
 #include "spinlock.h"     // For spinlock_t and functions
 #include "string.h"       // For strcmp, memset
 #include "types.h"        // For uintN_t types, bool, size_t
 #include "fs_errno.h"     // For error codes (FS_ERR_*, BLOCK_ERR_*)
 #include "libc/limits.h"  // For UINTPTR_MAX
 
 // <<<< NOTE: ASSERT calls have been removed from this version to allow linking. >>>>
 // <<<< It is STRONGLY recommended to implement a proper ASSERT mechanism     >>>>
 // <<<< for your kernel and re-introduce these checks.                       >>>>
 // #include <assert.h> // Include your assert header here when implemented.
 
 // --- ATA Register Definitions (Offsets from IO Base) ---
 #define ATA_REG_DATA         0 // Data Register (R/W: 16-bit access)
 #define ATA_REG_ERROR        1 // Error Register (R) / Features Register (W)
 #define ATA_REG_FEATURES     1 // Features Register (W)
 #define ATA_REG_SECCOUNT0    2 // Sector Count 0 (R/W) / LBA48 Low Sector Count (7:0)
 #define ATA_REG_LBA0         3 // LBA 0 (7:0)   / LBA48 Low Low Byte   (7:0)
 #define ATA_REG_LBA1         4 // LBA 1 (15:8)  / LBA48 Low Mid Byte   (15:8)
 #define ATA_REG_LBA2         5 // LBA 2 (23:16) / LBA48 Low High Byte  (23:16)
 #define ATA_REG_HDDEVSEL     6 // Drive/Head Select (R/W)
 #define ATA_REG_COMMAND      7 // Command Register (W)
 #define ATA_REG_STATUS       7 // Status Register (R)
 
 // LBA48 Registers (accessed via the same offsets after setting LBA48 mode/command)
 #define ATA_REG_SECCOUNT1    2 // Sector Count 1 (R/W) / LBA48 High Sector Count (15:8)
 #define ATA_REG_LBA3         3 // LBA 3 (31:24) / LBA48 Mid Low Byte   (31:24)
 #define ATA_REG_LBA4         4 // LBA 4 (39:32) / LBA48 Mid Mid Byte   (39:32)
 #define ATA_REG_LBA5         5 // LBA 5 (47:40) / LBA48 Mid High Byte  (47:40)
 
 // Alternate Status Register (Offset from Control Base)
 #define ATA_REG_ALTSTATUS    0 // Alternate Status (R, doesn't clear interrupt)
 #define ATA_REG_DEVCONTROL   0 // Device Control (W)
 
 // --- ATA Status Register Flags (ATA_REG_STATUS / ATA_REG_ALTSTATUS) ---
 #define ATA_SR_ERR  0x01 // Error flag set
 #define ATA_SR_IDX  0x02 // Index mark (obsolete)
 #define ATA_SR_CORR 0x04 // Corrected data (obsolete)
 #define ATA_SR_DRQ  0x08 // Data Request (ready for data transfer)
 #define ATA_SR_DSC  0x10 // Drive Seek Complete (obsolete)
 #define ATA_SR_DF   0x20 // Device Fault
 #define ATA_SR_DRDY 0x40 // Drive Ready (spin-up complete)
 #define ATA_SR_BSY  0x80 // Busy (processing command)
 
 // --- ATA Error Register Flags (ATA_REG_ERROR) ---
 #define ATA_ER_AMNF 0x01 // Address mark not found
 #define ATA_ER_TK0NF 0x02 // Track 0 not found
 #define ATA_ER_ABRT 0x04 // Aborted command
 #define ATA_ER_MCR  0x08 // Media change request
 #define ATA_ER_IDNF 0x10 // ID not found
 #define ATA_ER_MC   0x20 // Media changed
 #define ATA_ER_UNC  0x40 // Uncorrectable data error
 #define ATA_ER_BBK  0x80 // Bad block detected
 
 // --- ATA Commands ---
 #define ATA_CMD_IDENTIFY          0xEC // Identify Device
 #define ATA_CMD_SET_FEATURES      0xEF // Set Features
 #define ATA_CMD_SET_MULTIPLE      0xC6 // Set Multiple Mode
 #define ATA_CMD_READ_PIO          0x20 // Read Sectors (PIO, LBA28)
 #define ATA_CMD_READ_PIO_EXT      0x24 // Read Sectors (PIO, LBA48)
 #define ATA_CMD_READ_MULTIPLE     0xC4 // Read Multiple Sectors (PIO, LBA28)
 #define ATA_CMD_READ_MULTIPLE_EXT 0x29 // Read Multiple Sectors (PIO, LBA48)
 #define ATA_CMD_WRITE_PIO         0x30 // Write Sectors (PIO, LBA28)
 #define ATA_CMD_WRITE_PIO_EXT     0x34 // Write Sectors (PIO, LBA48)
 #define ATA_CMD_WRITE_MULTIPLE    0xC5 // Write Multiple Sectors (PIO, LBA28)
 #define ATA_CMD_WRITE_MULTIPLE_EXT 0x3A // Write Multiple Sectors (PIO, LBA48)
 #define ATA_CMD_FLUSH_CACHE       0xE7 // Write Cache Flush (LBA28)
 #define ATA_CMD_FLUSH_CACHE_EXT   0xEA // Write Cache Flush (LBA48)
 
 // --- Device Selection Bits (ATA_REG_HDDEVSEL) ---
 #define ATA_DEV_MASTER        0xA0 // Select Master Drive (Clear bit 4)
 #define ATA_DEV_SLAVE         0xB0 // Select Slave Drive (Set bit 4)
 #define ATA_DEV_LBA           0x40 // Use LBA addressing mode (Set bit 6)
 
 // --- Standard I/O Ports ---
 #define ATA_PRIMARY_IO        0x1F0
 #define ATA_PRIMARY_CTRL      0x3F6
 #define ATA_SECONDARY_IO      0x170
 #define ATA_SECONDARY_CTRL    0x376
 
 // --- Timeout ---
 // Increased timeout slightly for safety on slower emulators/hardware
 #define ATA_TIMEOUT_PIO       1500000 // Timeout loops for PIO status waits
 
 // --- Global Locks per Channel ---
 static spinlock_t g_ata_primary_lock;
 static spinlock_t g_ata_secondary_lock;
 
 // Define for detailed per-command logging (comment out to disable)
 // #define BLOCK_DEVICE_DEBUG
 
 // --- Wait Functions ---
 
 /**
  * @brief Polls the ATA status register waiting for specific conditions.
  * @param io_base The base I/O port for the channel.
  * @param wait_mask Mask of bits in the status register to check.
  * @param wait_value The expected value of the masked bits.
  * @param timeout Number of polling iterations before timeout.
  * @param context String describing the context for logging.
  * @return The final status byte on success or matching error, or -1 on timeout.
  */
 static int ata_poll_status(uint16_t io_base, uint8_t wait_mask, uint8_t wait_value, uint32_t timeout, const char* context) {
     uint32_t initial_timeout = timeout; // For logging
     while (timeout--) {
         uint8_t status = inb(io_base + ATA_REG_STATUS);
 
         if (!(wait_mask & (ATA_SR_ERR | ATA_SR_DF)) && (status & (ATA_SR_ERR | ATA_SR_DF))) {
             uint8_t err_reg = (status & ATA_SR_ERR) ? inb(io_base + ATA_REG_ERROR) : 0;
             terminal_printf("[ATA %s @%#x] Polling detected error: Status=%#x, Error=%#x\n", context, io_base, status, err_reg);
             return status;
         }
 
         if ((status & wait_mask) == wait_value) {
             return status;
         }
     }
 
     // Timeout occurred - *** CORRECTED FORMAT SPECIFIER FOR initial_timeout ***
     terminal_printf("[ATA %s @%#x] Poll timeout after %u loops (mask=%#x, val=%#x). Last status=%#x\n",
                       context, io_base, initial_timeout, wait_mask, wait_value, inb(io_base + ATA_REG_STATUS));
     return -1;
 }
 
 /**
  * @brief Provides a short delay (approx 400ns) by reading the alternate status register.
  * @param ctrl_base The control port base address for the channel.
  */
 static inline void ata_delay_400ns(uint16_t ctrl_base) {
     inb(ctrl_base + ATA_REG_ALTSTATUS);
     inb(ctrl_base + ATA_REG_ALTSTATUS);
     inb(ctrl_base + ATA_REG_ALTSTATUS);
     inb(ctrl_base + ATA_REG_ALTSTATUS);
 }
 
 // --- Core Operations ---
 
 /**
  * @brief Selects the specified drive on the channel and waits for it to be ready.
  * @param dev Pointer to the block_device_t structure.
  * @return BLOCK_ERR_OK on success, error code otherwise.
  */
 static int ata_select_drive(block_device_t *dev) {
     // ASSERT(dev != NULL); // Removed ASSERT
     if (!dev) return BLOCK_ERR_PARAMS; // Added NULL check instead
 
     int status = ata_poll_status(dev->io_base, ATA_SR_BSY | ATA_SR_DRQ, 0x00, ATA_TIMEOUT_PIO, "SelectWaitIdle");
     if (status < 0) return BLOCK_ERR_TIMEOUT;
     if (status & (ATA_SR_ERR | ATA_SR_DF)) {
         terminal_printf("[ATA Select %s] Error/Fault before select (Status=%#x).\n", dev->device_name, status);
         return BLOCK_ERR_DEV_FAULT;
     }
 
     outb(dev->io_base + ATA_REG_HDDEVSEL, (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA);
     ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
 
     status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SelectWaitBSY");
     if (status < 0) return BLOCK_ERR_TIMEOUT;
 
     status = inb(dev->io_base + ATA_REG_STATUS);
     if (!(status & ATA_SR_DRDY) || (status & (ATA_SR_ERR | ATA_SR_DF))) {
         terminal_printf("[ATA Select %s] Drive not ready/error after select (Status=%#x).\n", dev->device_name, status);
         if (!(status & ATA_SR_DRDY)) return BLOCK_ERR_NO_DEV;
         else return BLOCK_ERR_DEV_FAULT;
     }
     return BLOCK_ERR_OK;
 }
 
 /**
  * @brief Issues the IDENTIFY DEVICE command and parses key information.
  * @param dev Pointer to the block_device_t structure to populate.
  * @return BLOCK_ERR_OK on success, error code otherwise.
  */
 static int ata_identify(block_device_t *dev) {
     // ASSERT(dev != NULL); // Removed ASSERT
     if (!dev) return BLOCK_ERR_PARAMS; // Added NULL check instead
 
     int ret = ata_select_drive(dev);
     if (ret != BLOCK_ERR_OK) return ret;
 
     // Send IDENTIFY command
     outb(dev->io_base + ATA_REG_SECCOUNT0, 0);
     outb(dev->io_base + ATA_REG_LBA0, 0);
     outb(dev->io_base + ATA_REG_LBA1, 0);
     outb(dev->io_base + ATA_REG_LBA2, 0);
     outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
     ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
 
     uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
     if (status == 0 || status == 0xFF) {
         terminal_printf("[ATA IDENTIFY %s] No device detected (Status=%#x).\n", dev->device_name, status);
         return BLOCK_ERR_NO_DEV;
     }
 
     // Wait for BSY clear
     status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "IdentifyBSY");
     if (status < 0) return BLOCK_ERR_TIMEOUT;
     if (status & (ATA_SR_ERR | ATA_SR_DF)) {
          terminal_printf("[ATA IDENTIFY %s] Error/Fault after command (Status=%#x).\n", dev->device_name, status);
          return BLOCK_ERR_DEV_FAULT;
     }
 
     // Wait for DRQ set (or error)
     status = ata_poll_status(dev->io_base, ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF, ATA_SR_DRQ, ATA_TIMEOUT_PIO, "IdentifyDRQ");
     if (status < 0) return BLOCK_ERR_TIMEOUT;
     if (status & (ATA_SR_ERR | ATA_SR_DF)) {
          terminal_printf("[ATA IDENTIFY %s] Error/Fault waiting for data (Status=%#x).\n", dev->device_name, status);
          return BLOCK_ERR_DEV_FAULT;
     }
 
     // Read IDENTIFY data
     uint16_t identify_data[256];
     for (int i = 0; i < 256; i++) {
         identify_data[i] = inw(dev->io_base + ATA_REG_DATA);
     }
 
     // Parse data
     dev->sector_size = 512;
     if (!(identify_data[49] & (1 << 9))) {
         terminal_printf("[ATA IDENTIFY %s] Error: LBA addressing not supported.\n", dev->device_name);
         return BLOCK_ERR_UNSUPPORTED;
     }
 
     dev->lba48_supported = (identify_data[83] & (1 << 10)) != 0;
     if (dev->lba48_supported) {
         dev->total_sectors = *(uint64_t*)&identify_data[100];
     } else {
         dev->total_sectors = *(uint32_t*)&identify_data[60];
     }
     if (dev->total_sectors == 0) {
         terminal_printf("[ATA IDENTIFY %s] Error: Reported total sectors is zero.\n", dev->device_name);
         return BLOCK_ERR_NO_DEV;
     }
 
     dev->multiple_sector_count = 0;
     if (identify_data[88] & 0x0001) {
         uint16_t mult_count = identify_data[47] & 0x00FF;
         if (mult_count > 0 && mult_count <= 16) {
             dev->multiple_sector_count = mult_count;
             terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode (Preferred Count=%u)\n", dev->device_name, dev->multiple_sector_count);
         } else if (mult_count > 0) {
             terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode but count %u > 16, ignoring.\n", dev->device_name, mult_count);
         }
     }
     return BLOCK_ERR_OK;
 }
 
 /**
  * @brief Attempts to set the MULTIPLE sector mode count on the device.
  * @param dev Pointer to the block_device_t structure (must have been identified).
  * @return BLOCK_ERR_OK on success or if MULTIPLE mode is not supported/enabled.
  * Returns error code on failure to set the mode.
  */
 static int ata_set_multiple_mode(block_device_t *dev) {
     // ASSERT(dev != NULL); // Removed ASSERT
      if (!dev) return BLOCK_ERR_PARAMS; // Added NULL check instead
 
     if (dev->multiple_sector_count == 0 || dev->multiple_sector_count > 16) {
         return BLOCK_ERR_OK; // Nothing valid to set
     }
     int ret = ata_select_drive(dev);
     if (ret != BLOCK_ERR_OK) return ret;
 
     outb(dev->io_base + ATA_REG_SECCOUNT0, dev->multiple_sector_count);
     outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
     ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
 
     int status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SetMultipleBSY");
     if (status < 0) return BLOCK_ERR_TIMEOUT;
 
     status = inb(dev->io_base + ATA_REG_STATUS);
     if (!(status & ATA_SR_DRDY) || (status & (ATA_SR_ERR | ATA_SR_DF))) {
         terminal_printf("[ATA %s] Error setting MULTIPLE mode (Status=%#x), disabling feature.\n", dev->device_name, status);
         dev->multiple_sector_count = 0;
         return BLOCK_ERR_OK; // Non-fatal
     }
     terminal_printf("[ATA %s] MULTIPLE mode SET to %u sectors.\n", dev->device_name, dev->multiple_sector_count);
     return BLOCK_ERR_OK;
 }
 
 /**
  * @brief Sets up the LBA address and sector count registers for a PIO command.
  * @param dev Pointer to the block device.
  * @param lba Starting LBA address.
  * @param count Number of sectors for this command (must be pre-clamped, 1-256 for LBA28, 1-65536 for LBA48).
  */
 static void ata_setup_lba(block_device_t *dev, uint64_t lba, size_t count) {
     // ASSERT(dev != NULL); // Removed ASSERT
     // ASSERT(count > 0); // Removed ASSERT
      if (!dev || count == 0) return; // Basic checks
 
     uint8_t dev_select_base = (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA;
     bool needs_lba48 = dev->lba48_supported && (lba >= 0x10000000ULL || count > 256);
 
     if (needs_lba48) {
         // LBA48 Mode
         // ASSERT(count <= 65536); // Removed ASSERT
         uint16_t sector_count_reg = (count >= 65536) ? 0 : (uint16_t)count; // 0 means 65536
 
         outb(dev->io_base + ATA_REG_SECCOUNT1, (uint8_t)(sector_count_reg >> 8));
         outb(dev->io_base + ATA_REG_LBA3,      (uint8_t)(lba >> 24));
         outb(dev->io_base + ATA_REG_LBA4,      (uint8_t)(lba >> 32));
         outb(dev->io_base + ATA_REG_LBA5,      (uint8_t)(lba >> 40));
         outb(dev->io_base + ATA_REG_SECCOUNT0, (uint8_t)(sector_count_reg & 0xFF));
         outb(dev->io_base + ATA_REG_LBA0,      (uint8_t)(lba & 0xFF));
         outb(dev->io_base + ATA_REG_LBA1,      (uint8_t)(lba >> 8));
         outb(dev->io_base + ATA_REG_LBA2,      (uint8_t)(lba >> 16));
     } else {
         // LBA28 Mode
         // ASSERT(count <= 256); // Removed ASSERT
         // ASSERT(lba < 0x10000000ULL); // Removed ASSERT
         uint8_t sector_count_reg = (count >= 256) ? 0 : (uint8_t)count; // 0 means 256
 
         uint8_t dev_select = dev_select_base | ((lba >> 24) & 0x0F);
         outb(dev->io_base + ATA_REG_HDDEVSEL, dev_select);
 
         outb(dev->io_base + ATA_REG_SECCOUNT0, sector_count_reg);
         outb(dev->io_base + ATA_REG_LBA0,      (uint8_t)(lba & 0xFF));
         outb(dev->io_base + ATA_REG_LBA1,      (uint8_t)((lba >> 8) & 0xFF));
         outb(dev->io_base + ATA_REG_LBA2,      (uint8_t)((lba >> 16) & 0xFF));
     }
 }
 
 /**
  * @brief Performs the PIO data transfer for a block of sectors.
  * @param dev Pointer to the block device.
  * @param buffer Pointer to the data buffer for this block.
  * @param sectors_in_block Number of sectors to transfer in this single command.
  * @param write True if writing, false if reading.
  * @return BLOCK_ERR_OK on success, error code otherwise.
  */
 static int ata_pio_transfer_block(block_device_t *dev, void *buffer, size_t sectors_in_block, bool write) {
     // ASSERT(dev != NULL); // Removed ASSERT
     // ASSERT(buffer != NULL); // Removed ASSERT
     // ASSERT(sectors_in_block > 0); // Removed ASSERT
     // ASSERT(dev->sector_size > 0 && (dev->sector_size % 2 == 0)); // Removed ASSERT
      if (!dev || !buffer || sectors_in_block == 0 || dev->sector_size == 0 || (dev->sector_size % 2 != 0)) {
          return BLOCK_ERR_PARAMS; // Basic checks
      }
 
     int ret = BLOCK_ERR_OK;
     size_t words_per_sector = dev->sector_size / 2;
     uint16_t data_port = dev->io_base + ATA_REG_DATA;
 
     for (size_t sector = 0; sector < sectors_in_block; sector++) {
         // 1. Wait for DRQ (or error)
         ret = ata_poll_status(dev->io_base, ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF, ATA_SR_DRQ, ATA_TIMEOUT_PIO, "Data DRQ");
         if (ret < 0) return BLOCK_ERR_TIMEOUT;
         if (ret & (ATA_SR_ERR | ATA_SR_DF)) {
             uint8_t err_reg = (ret & ATA_SR_ERR) ? inb(dev->io_base + ATA_REG_ERROR) : 0;
             terminal_printf("[ATA %s IO] Error/Fault before sector %zu transfer (Status=%#x, Error=%#x)\n",
                               dev->device_name, sector, ret, err_reg);
             return (ret & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
         }
 
         // 2. Transfer Data (one sector)
         uint16_t *buf_words = (uint16_t *)((uint8_t *)buffer + sector * dev->sector_size);
 
         if (write) {
             for (size_t i = 0; i < words_per_sector; i++) {
                 outw(data_port, buf_words[i]);
             }
         } else { // Read
             for (size_t i = 0; i < words_per_sector; i++) {
                 buf_words[i] = inw(data_port);
             }
         }
         ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
     }
 
     // 3. Final check after ALL sectors transferred
     ret = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Post-Tx BSY Clear");
     if (ret < 0) return BLOCK_ERR_TIMEOUT;
 
     if (ret & (ATA_SR_ERR | ATA_SR_DF)) {
          uint8_t err_reg = (ret & ATA_SR_ERR) ? inb(dev->io_base + ATA_REG_ERROR) : 0;
          terminal_printf("[ATA %s IO] Error/Fault after transfer complete (Status=%#x, Error=%#x)\n",
                            dev->device_name, ret, err_reg);
         return (ret & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
     }
 
     return BLOCK_ERR_OK;
 }
 
 
 // --- Public API ---
 
 /**
  * @brief Initializes the ATA channel locks. Call once during kernel init.
  */
 void ata_channels_init(void) {
     spinlock_init(&g_ata_primary_lock);
     spinlock_init(&g_ata_secondary_lock);
     terminal_write("[ATA] Channel locks initialized.\n");
 }
 
 /**
  * @brief Initializes a block_device_t structure for a given device name (e.g., "hda").
  * Probes the device, identifies it, and sets MULTIPLE mode if supported.
  *
  * @param device The device name string ("hda", "hdb", "hdc", "hdd").
  * @param dev Pointer to the block_device_t structure to initialize.
  * @return BLOCK_ERR_OK on success, error code otherwise.
  */
 int block_device_init(const char *device, block_device_t *dev) {
     if (!device || !dev) return BLOCK_ERR_PARAMS;
 
     memset(dev, 0, sizeof(block_device_t));
     dev->device_name = device;
 
     bool primary_channel = true;
     bool is_slave = false;
     if (strcmp(device, "hda") == 0) { /* Primary Master */ }
     else if (strcmp(device, "hdb") == 0) { is_slave = true; /* Primary Slave */ }
     else if (strcmp(device, "hdc") == 0) { primary_channel = false; /* Secondary Master */ }
     else if (strcmp(device, "hdd") == 0) { primary_channel = false; is_slave = true; /* Secondary Slave */ }
     else {
         terminal_printf("[BlockDev Init] Error: Unknown device name '%s'.\n", device);
         return BLOCK_ERR_PARAMS;
     }
 
     dev->io_base = primary_channel ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
     dev->control_base = primary_channel ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
     dev->is_slave = is_slave;
     dev->channel_lock = primary_channel ? &g_ata_primary_lock : &g_ata_secondary_lock;
 
     terminal_printf("[BlockDev Init] Probing '%s' (IO:%#x, Ctrl:%#x, Slave:%d)...\n",
                       device, dev->io_base, dev->control_base, dev->is_slave);
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);
 
     int ret = ata_identify(dev);
     if (ret == BLOCK_ERR_OK) {
         ret = ata_set_multiple_mode(dev);
         if (ret != BLOCK_ERR_OK) {
             terminal_printf("[BlockDev Init] Warning: Failed to set MULTIPLE mode for '%s' (err %d), continuing without it.\n", device, ret);
             ret = BLOCK_ERR_OK;
         }
     }
 
     dev->initialized = (ret == BLOCK_ERR_OK);
 
     spinlock_release_irqrestore(dev->channel_lock, irq_flags);
 
     if (!dev->initialized) {
         terminal_printf("[BlockDev Init] Failed for '%s' during IDENTIFY (err %d).\n", device, ret);
         return ret;
     }
 
     // *** CORRECTED FORMAT SPECIFIER for sector_size ***
     terminal_printf("[BlockDev Init] OK: '%s' LBA48:%d Sectors:%u Mult:%u SectorSize:%lu\n",
                       device,
                       dev->lba48_supported,
                       dev->total_sectors,
                       dev->multiple_sector_count,
                       (unsigned long)dev->sector_size); // Use %lu assuming uint32_t is long unsigned
 
     return BLOCK_ERR_OK;
 }
 
 /**
  * @brief Reads or writes sectors to/from a block device using PIO.
  * Internal implementation handling locking, LBA modes, and MULTIPLE mode.
  *
  * @param dev Pointer to the initialized block_device_t structure.
  * @param lba The starting logical block address (sector number).
  * @param buffer Pointer to the data buffer (must be large enough for 'count' sectors).
  * @param count The number of sectors to transfer.
  * @param write True for writing, false for reading.
  * @return BLOCK_ERR_OK (0) on success, or a negative BLOCK_ERR_* code on failure.
  */
 int block_device_transfer(block_device_t *dev, uint64_t lba, void *buffer, size_t count, bool write) {
     // 1. Validate Parameters
     if (!dev || !dev->initialized || !buffer || count == 0) {
         // *** CORRECTED Format *** (Simplified message as compiler warning was confusing)
          terminal_printf("[ATA %s RW] Error: Invalid parameters provided.\n", dev ? dev->device_name : "N/A");
         return BLOCK_ERR_PARAMS;
     }
     if (dev->sector_size == 0 || (dev->sector_size % 2 != 0)) {
          // *** CORRECTED FORMAT: Use %lu for sector_size ***
          terminal_printf("[ATA %s RW] Error: Invalid sector size (%u) in device struct.\n",
                          dev->device_name, (unsigned long)dev->sector_size);
          // *** FIXED ERROR CODE ***
          return BLOCK_ERR_PARAMS;
     }
     if (lba >= dev->total_sectors || count > dev->total_sectors - lba) {
         // *** CORRECTED FORMAT: %u, %zu, %u ***
         terminal_printf("[ATA %s RW] Error: LBA %u + Count %zu out of bounds (Total %u).\n",
                           dev->device_name, lba, count, dev->total_sectors);
         return BLOCK_ERR_BOUNDS;
     }
 
     // 2. Acquire Lock
     uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);
     int final_ret = BLOCK_ERR_OK;
     size_t sectors_remaining = count;
     uint64_t current_lba = lba;
     uint8_t *current_buffer = (uint8_t *)buffer;
 
     // 3. Transfer Loop
     while (sectors_remaining > 0) {
         int current_ret = BLOCK_ERR_OK;
 
         // --- Determine parameters for this command ---
         bool use_lba48 = dev->lba48_supported && (current_lba + sectors_remaining > 0x10000000ULL);
         size_t max_sectors_per_ata_cmd = use_lba48 ? 65536 : 256;
         bool try_multiple = (dev->multiple_sector_count > 0) && (sectors_remaining >= dev->multiple_sector_count);
 
         size_t sectors_this_cmd;
         uint8_t command;
 
         if (try_multiple) sectors_this_cmd = dev->multiple_sector_count;
         else              sectors_this_cmd = 1;
 
         if (sectors_this_cmd > sectors_remaining) sectors_this_cmd = sectors_remaining;
         if (sectors_this_cmd > max_sectors_per_ata_cmd) sectors_this_cmd = max_sectors_per_ata_cmd;
 
         bool use_multiple_this_cmd = try_multiple && (sectors_this_cmd >= dev->multiple_sector_count);
         if (try_multiple && !use_multiple_this_cmd) {
              sectors_this_cmd = 1;
              if (sectors_this_cmd > sectors_remaining) sectors_this_cmd = sectors_remaining;
         }
 
         if (write) {
             if (use_multiple_this_cmd) command = use_lba48 ? ATA_CMD_WRITE_MULTIPLE_EXT : ATA_CMD_WRITE_MULTIPLE;
             else                       command = use_lba48 ? ATA_CMD_WRITE_PIO_EXT      : ATA_CMD_WRITE_PIO;
         } else {
             if (use_multiple_this_cmd) command = use_lba48 ? ATA_CMD_READ_MULTIPLE_EXT  : ATA_CMD_READ_MULTIPLE;
             else                       command = use_lba48 ? ATA_CMD_READ_PIO_EXT       : ATA_CMD_READ_PIO;
         }
 
         if (!use_lba48 && (current_lba + sectors_this_cmd > 0x10000000ULL)) {
             terminal_printf("[ATA %s RW] Error: LBA28 command exceeds address limit (LBA %u, Count %zu).\n", dev->device_name, current_lba, sectors_this_cmd);
             final_ret = BLOCK_ERR_BOUNDS;
             break;
         }
 
         // --- Execute Single ATA Command ---
         current_ret = ata_select_drive(dev);
         if (current_ret != BLOCK_ERR_OK) {
             terminal_printf("[ATA %s RW %s] Select drive failed (LBA %u, Err %d)\n",
                               dev->device_name, write ? "Write" : "Read", current_lba, current_ret);
             final_ret = current_ret;
             break;
         }
 
         ata_setup_lba(dev, current_lba, sectors_this_cmd);
 
         #ifdef BLOCK_DEVICE_DEBUG
         terminal_printf("[ATA %s Transfer] %s: LBA=%u, Count=%zu, Buf=%p, Cmd=%#x, Mult=%d, LBA48=%d\n",
                         dev->device_name, write ? "WR" : "RD", current_lba, sectors_this_cmd, (void*)current_buffer,
                         command, use_multiple_this_cmd, use_lba48);
         #endif
 
         outb(dev->io_base + ATA_REG_COMMAND, command);
         ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
 
         current_ret = ata_pio_transfer_block(dev, current_buffer, sectors_this_cmd, write);
         if (current_ret != BLOCK_ERR_OK) {
             terminal_printf("[ATA %s RW %s] Transfer block failed (Cmd %#x, LBA %u, Count %zu, Err %d)\n",
                               dev->device_name, write ? "Write" : "Read", command, current_lba, sectors_this_cmd, current_ret);
             final_ret = current_ret;
             break;
         }
 
         // Advance state
         sectors_remaining -= sectors_this_cmd;
         current_lba += sectors_this_cmd;
         uintptr_t buffer_advance = sectors_this_cmd * dev->sector_size;
         if ((uintptr_t)current_buffer > UINTPTR_MAX - buffer_advance) {
              terminal_printf("[ATA %s RW] Error: Buffer pointer overflow during transfer.\n", dev->device_name);
              final_ret = FS_ERR_UNKNOWN;
              break;
         }
         current_buffer += buffer_advance;
 
     } // End while(sectors_remaining > 0)
 
     // 4. Final Cache Flush
     if (write && final_ret == BLOCK_ERR_OK) {
         int sel_ret = ata_select_drive(dev);
         if (sel_ret == BLOCK_ERR_OK) {
             uint8_t flush_cmd = dev->lba48_supported ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
             outb(dev->io_base + ATA_REG_COMMAND, flush_cmd);
             ata_delay_400ns(dev->control_base + ATA_REG_ALTSTATUS);
 
             int flush_status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO * 3, "FlushCache");
             if (flush_status < 0) {
                  terminal_printf("[ATA %s RW Write] FlushCache timeout.\n", dev->device_name);
                  final_ret = BLOCK_ERR_TIMEOUT;
             } else if (flush_status & (ATA_SR_ERR | ATA_SR_DF)) {
                  uint8_t err_reg = (flush_status & ATA_SR_ERR) ? inb(dev->io_base + ATA_REG_ERROR) : 0;
                  terminal_printf("[ATA %s RW Write] FlushCache error/fault (Status=%#x, Error=%#x).\n", dev->device_name, flush_status, err_reg);
                  final_ret = BLOCK_ERR_DEV_FAULT;
             }
         } else {
              terminal_printf("[ATA %s RW Write] Select drive failed before FlushCache (Err %d).\n", dev->device_name, sel_ret);
              if (final_ret == BLOCK_ERR_OK) final_ret = sel_ret;
         }
     }
 
     // 5. Release Lock and Return
     spinlock_release_irqrestore(dev->channel_lock, irq_flags);
     return final_ret;
 }
 
 /**
  * @brief Reads sectors from the block device.
  * Wrapper around block_device_transfer.
  */
 int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count) {
     return block_device_transfer(dev, lba, buffer, count, false);
 }
 
 /**
  * @brief Writes sectors to the block device.
  * Wrapper around block_device_transfer.
  */
 int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count) {
     return block_device_transfer(dev, lba, (void *)buffer, count, true);
 }
 
 // === END: Improved block_device.c (v5 - ASSERTs Removed) ===