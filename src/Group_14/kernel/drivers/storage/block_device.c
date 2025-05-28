/**
 * @file block_device.c
 * @brief ATA PIO Block Device Driver (IRQ for R/W with Polling Assist, Polling for IDENTIFY)
 *
 * Author: Group 14 (UiA) & Gemini
 * Version: 5.4 - Fixed build errors from debug prints and unused variables.
 */

 #include <kernel/drivers/storage/block_device.h>
 #include <kernel/drivers/storage/buffer_cache.h> // <<< ADD THIS INCLUDE
 #include <kernel/lib/port_io.h>      // For inb, outb, inw, outw
 #include <kernel/drivers/display/terminal.h>     // For terminal_printf/write
 #include <kernel/sync/spinlock.h>     // For spinlock_t and functions
 #include <kernel/lib/string.h>       // For strcmp, memset
 #include <kernel/core/types.h>        // For uintN_t types, bool, size_t
 #include <kernel/fs/vfs/fs_errno.h>     // For error codes (FS_ERR_*, BLOCK_ERR_*)
 #include <libc/limits.h>  // For UINTPTR_MAX
 #include <kernel/cpu/isr_frame.h>    // Include the frame definition
 #include <kernel/lib/assert.h>       // KERNEL_ASSERT (Optional, but recommended)
 #include <kernel/drivers/input/keyboard_hw.h> // <<< ADDED for KBC_STATUS_PORT constant for debug prints
 // --- ATA Register Definitions ---
 #define ATA_REG_DATA        0
 #define ATA_REG_ERROR        1
 #define ATA_REG_FEATURES     1
 #define ATA_REG_SECCOUNT0    2
 #define ATA_REG_LBA0         3
 #define ATA_REG_LBA1         4
 #define ATA_REG_LBA2         5
 #define ATA_REG_HDDEVSEL     6
 #define ATA_REG_COMMAND      7
 #define ATA_REG_STATUS       7
 #define ATA_REG_SECCOUNT1    2 // LBA48
 #define ATA_REG_LBA3         3 // LBA48
 #define ATA_REG_LBA4         4 // LBA48
 #define ATA_REG_LBA5         5 // LBA48
 #define ATA_REG_ALTSTATUS    0 // Control Base Offset
 #define ATA_REG_DEVCONTROL   0 // Control Base Offset

 // --- ATA Status Register Flags ---
 #define ATA_SR_ERR  0x01 // Error
 #define ATA_SR_DRQ  0x08 // Data Request
 #define ATA_SR_DF   0x20 // Device Fault
 #define ATA_SR_DRDY 0x40 // Drive Ready
 #define ATA_SR_BSY  0x80 // Busy

 // --- ATA Commands ---
 #define ATA_CMD_IDENTIFY          0xEC
 #define ATA_CMD_SET_MULTIPLE      0xC6
 #define ATA_CMD_READ_PIO          0x20
 #define ATA_CMD_READ_PIO_EXT      0x24
 #define ATA_CMD_READ_MULTIPLE     0xC4
 #define ATA_CMD_READ_MULTIPLE_EXT 0x29
 #define ATA_CMD_WRITE_PIO         0x30
 #define ATA_CMD_WRITE_PIO_EXT     0x34
 #define ATA_CMD_WRITE_MULTIPLE    0xC5
 #define ATA_CMD_WRITE_MULTIPLE_EXT 0x3A
 #define ATA_CMD_FLUSH_CACHE       0xE7
 #define ATA_CMD_FLUSH_CACHE_EXT   0xEA

 // --- Device Selection Bits ---
 #define ATA_DEV_MASTER        0xA0
 #define ATA_DEV_SLAVE         0xB0
 #define ATA_DEV_LBA           0x40

 // --- Standard I/O Ports ---
 #define ATA_PRIMARY_IO        0x1F0
 #define ATA_PRIMARY_CTRL      0x3F6
 #define ATA_SECONDARY_IO      0x170
 #define ATA_SECONDARY_CTRL    0x376

 // --- Timeout Values ---
 #define ATA_TIMEOUT_PIO        1500000 // Base timeout loops for polling status waits
 #define ATA_IRQ_WAIT_MULTIPLIER   20   // Multiplier for IRQ wait loop

 // --- Global Locks per Channel ---
 static spinlock_t g_ata_primary_lock;
 static spinlock_t g_ata_secondary_lock;

 // IRQ Handling Flags (Specific to Primary Channel for now)
 static volatile bool g_ata_primary_irq_fired = false;
 static volatile uint8_t g_ata_primary_last_status = 0;
 static volatile uint8_t g_ata_primary_last_error = 0;

 // --- Internal Helper Prototypes ---
 static int ata_poll_status(uint16_t io_base, uint8_t wait_mask, uint8_t wait_value, uint32_t timeout, const char* context);
 static void ata_delay_400ns(uint16_t ctrl_base);
 static int ata_select_drive(block_device_t *dev);
 static int ata_identify(block_device_t *dev); // Uses polling
 static int ata_set_multiple_mode(block_device_t *dev); // Uses polling
 static void ata_setup_lba(block_device_t *dev, uint64_t lba, size_t count);
 static int ata_pio_transfer_block(block_device_t *dev, void *buffer, size_t sectors_in_block, bool write);
 static int block_device_transfer(block_device_t *dev, uint64_t lba, void *buffer, size_t count, bool write); // Uses IRQ wait

 // --- Wait Functions ---

 /**
  * @brief Polls the ATA status register waiting for specific conditions (Non-IRQ method).
  */
 static int ata_poll_status(uint16_t io_base, uint8_t wait_mask, uint8_t wait_value, uint32_t timeout, const char* context) {
     uint32_t initial_timeout = timeout;
     while (timeout--) {
         uint8_t status = inb(io_base + ATA_REG_STATUS);
         if (!(wait_mask & (ATA_SR_ERR | ATA_SR_DF)) && (status & (ATA_SR_ERR | ATA_SR_DF))) {
             uint8_t err_reg = (status & ATA_SR_ERR) ? inb(io_base + ATA_REG_ERROR) : 0;
             terminal_printf("[ATA Polling %s @%#x] Error: Status=%#x, Error=%#x\n", context, io_base, status, err_reg);
             return status;
         }
         if ((status & wait_mask) == wait_value) {
             return status;
         }
         // asm volatile("pause"); // Can add pause if needed
     }
     // <<< FIX: Use %lu for uint32_t (initial_timeout) >>>
     terminal_printf("[ATA Polling %s @%#x] Poll timeout after %lu loops (mask=%#x, val=%#x). Last status=%#x\n",
                     context, io_base, (unsigned long)initial_timeout, wait_mask, wait_value, inb(io_base + ATA_REG_STATUS));
     return -1; // Indicate timeout
 }

 /**
  * @brief Provides a short delay (~400ns) by reading the alternate status register.
  */
 static inline void ata_delay_400ns(uint16_t ctrl_base) {
     (void)inb(ctrl_base + ATA_REG_ALTSTATUS);
     (void)inb(ctrl_base + ATA_REG_ALTSTATUS);
     (void)inb(ctrl_base + ATA_REG_ALTSTATUS);
     (void)inb(ctrl_base + ATA_REG_ALTSTATUS);
 }

 // --- Core Operations --- (ata_select_drive, ata_identify, ata_set_multiple_mode, ata_setup_lba remain unchanged)
 /**
  * @brief Selects the specified drive on the channel and waits for it to be ready using polling.
  */
 static int ata_select_drive(block_device_t *dev) {
     KERNEL_ASSERT(dev != NULL, "NULL dev in ata_select_drive");

     // 1. Wait for BSY=0 before selecting.
     int poll_result = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SelectWaitIdle");
     if (poll_result < 0) {
         terminal_printf("[ATA Select %s] Timeout waiting for BSY=0 before select.\n", dev->device_name);
         return BLOCK_ERR_TIMEOUT;
     }
     uint8_t status = (uint8_t)poll_result;
     if (status & (ATA_SR_ERR | ATA_SR_DF)) {
         terminal_printf("[ATA Select %s] Error/Fault detected before select (Status=%#x).\n", dev->device_name, status);
         return BLOCK_ERR_DEV_FAULT;
     }

     // 2. Select drive & LBA mode.
     uint8_t drive_select_command = (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA;
     outb(dev->io_base + ATA_REG_HDDEVSEL, drive_select_command);

     // 3. Wait 400ns.
     ata_delay_400ns(dev->control_base);

     // 4. Poll again for BSY=0 after selection.
     poll_result = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SelectWaitBSY");
     if (poll_result < 0) {
         terminal_printf("[ATA Select %s] Timeout waiting for BSY=0 after select.\n", dev->device_name);
         return BLOCK_ERR_TIMEOUT;
     }

     // 5. Read final status and check DRDY=1 and ERR=0, DF=0.
     status = inb(dev->io_base + ATA_REG_STATUS);
     if (!(status & ATA_SR_DRDY)) {
         terminal_printf("[ATA Select %s] Drive not ready after select (Status=%#x, DRDY=0).\n", dev->device_name, status);
         return BLOCK_ERR_NO_DEV;
     }
     if (status & (ATA_SR_ERR | ATA_SR_DF)) {
         terminal_printf("[ATA Select %s] Drive error/fault after select (Status=%#x).\n", dev->device_name, status);
         return BLOCK_ERR_DEV_FAULT;
     }

     return BLOCK_ERR_OK;
 }

 /**
 * @brief Issues the IDENTIFY DEVICE command and parses key information using polling.
 * Includes fix for parsing logical sector size from words 117-118.
 */
static int ata_identify(block_device_t *dev) {
    KERNEL_ASSERT(dev != NULL, "NULL dev in ata_identify");

    int ret = ata_select_drive(dev);
    if (ret != BLOCK_ERR_OK) return ret;

    // Send IDENTIFY command
    outb(dev->io_base + ATA_REG_SECCOUNT0, 0);
    outb(dev->io_base + ATA_REG_LBA0, 0);
    outb(dev->io_base + ATA_REG_LBA1, 0);
    outb(dev->io_base + ATA_REG_LBA2, 0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(dev->control_base);

    // Check initial status after command
    uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        terminal_printf("[ATA IDENTIFY %s] No device detected (Status=%#x).\n", dev->device_name, status);
        return BLOCK_ERR_NO_DEV;
    }

    // --- Poll for completion ---
    ret = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "IdentifyBSYClear");
    if (ret < 0) return BLOCK_ERR_TIMEOUT;
    status = (uint8_t)ret;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        terminal_printf("[ATA IDENTIFY %s] Error/Fault after command (Status=%#x).\n", dev->device_name, status);
        return BLOCK_ERR_DEV_FAULT;
    }
    ret = ata_poll_status(dev->io_base, ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF, ATA_SR_DRQ, ATA_TIMEOUT_PIO, "IdentifyDRQSet");
    if (ret < 0) return BLOCK_ERR_TIMEOUT;
    status = (uint8_t)ret;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        terminal_printf("[ATA IDENTIFY %s] Error/Fault waiting for data (Status=%#x).\n", dev->device_name, status);
        return BLOCK_ERR_DEV_FAULT;
    }
    if (!(status & ATA_SR_DRQ)) {
        terminal_printf("[ATA IDENTIFY %s] Polling finished but DRQ not set! (Status=%#x).\n", dev->device_name, status);
        return BLOCK_ERR_IO;
    }

    // Read IDENTIFY data (512 bytes = 256 words)
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) { identify_data[i] = inw(dev->io_base + ATA_REG_DATA); }

    // Final status check after reading data
    ret = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "IdentifyPostReadBSYClear");
    if (ret < 0) return BLOCK_ERR_TIMEOUT;
    status = (uint8_t)ret;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        terminal_printf("[ATA IDENTIFY %s] Error/Fault after reading data (Status=%#x).\n", dev->device_name, status);
        return BLOCK_ERR_DEV_FAULT;
    }

    // --- Parse data ---

    // Logical Sector Size (Word 106 indicates support, Words 117-118 contain size if supported)
    if ((identify_data[106] & 0x4000) && !(identify_data[106] & 0x8000)) { // Check bit 14 set, bit 15 clear
        // Combine words 117 (low) and 118 (high) into a 32-bit value representing size in words
        uint32_t logical_sector_size_words = identify_data[117] | ((uint32_t)identify_data[118] << 16);
        uint32_t logical_sector_size_bytes = logical_sector_size_words * 2; // Convert words to bytes

        // Sanity checks: must be power-of-two and within reasonable bounds
        if (logical_sector_size_bytes >= 512 &&
            logical_sector_size_bytes <= MAX_BUFFER_BLOCK_SIZE && // Use defined max
            (logical_sector_size_bytes & (logical_sector_size_bytes - 1)) == 0) // Check power-of-two
        {
            dev->sector_size = logical_sector_size_bytes;
            terminal_printf("[ATA IDENTIFY %s] Logical Sector Size supported: %u bytes.\n",
                             dev->device_name, dev->sector_size);
        } else {
            terminal_printf("[ATA IDENTIFY %s] Warning: Reported logical sector size (%u bytes) invalid or unsupported by cache. Defaulting to 512.\n",
                             dev->device_name, logical_sector_size_bytes);
            dev->sector_size = 512;
        }
    } else {
        // Default to 512 if feature not indicated by word 106
        terminal_printf("[ATA IDENTIFY %s] Logical Sector Size > 512 bytes not indicated. Defaulting to 512.\n", dev->device_name);
        dev->sector_size = 512;
    }

    // LBA Support (Word 49, bit 9)
    if (!(identify_data[49] & (1 << 9))) {
        terminal_printf("[ATA IDENTIFY %s] Error: LBA addressing not supported.\n", dev->device_name);
        return BLOCK_ERR_UNSUPPORTED;
    }

    // LBA48 Support (Word 83, bit 10)
    dev->lba48_supported = (identify_data[83] & (1 << 10)) != 0;

    // Total Sectors (Words 100-103 for LBA48, Words 60-61 for LBA28)
    // Note: Using direct cast relies on compiler handling potential unaligned access on some archs.
    // Safer approach might be memcpy into a local uint64_t/uint32_t.
    dev->total_sectors = dev->lba48_supported ?
                         (*(uint64_t*)&identify_data[100]) :
                         (*(uint32_t*)&identify_data[60]);
    if (dev->total_sectors == 0) {
        terminal_printf("[ATA IDENTIFY %s] Error: Reported total sectors is zero.\n", dev->device_name);
        return BLOCK_ERR_NO_DEV; // Or BLOCK_ERR_INVALID_FORMAT
    }

    // Multiple Sector Mode (Word 47, low byte = count, Word 88 bit 0 = supported)
    // Changed: Check Word 88 bit 0 instead of Word 47 bit 8 for support indication
    dev->multiple_sector_count = 0;
    if (identify_data[88] & 0x0001) { // Check if "multiple sector setting is valid" bit is set
        uint16_t mult_count = identify_data[47] & 0x00FF; // Count is in low byte of Word 47
        if (mult_count > 0 && mult_count <= 16) { // Standard allows up to 16
            dev->multiple_sector_count = mult_count;
            terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode (Preferred Count=%u)\n", dev->device_name, dev->multiple_sector_count);
        } else if (mult_count > 0) {
             terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode but count %u > 16, ignoring.\n", dev->device_name, mult_count);
        }
    }

    return BLOCK_ERR_OK;
}

 /**
  * @brief Attempts to set the MULTIPLE sector mode count on the device using polling.
  */
 static int ata_set_multiple_mode(block_device_t *dev) {
      KERNEL_ASSERT(dev != NULL, "NULL dev in ata_set_multiple_mode");
      if (dev->multiple_sector_count == 0 || dev->multiple_sector_count > 16) return BLOCK_ERR_OK;
      int ret = ata_select_drive(dev);
      if (ret != BLOCK_ERR_OK) return ret;
      outb(dev->io_base + ATA_REG_SECCOUNT0, dev->multiple_sector_count);
      outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
      ata_delay_400ns(dev->control_base);
      int poll_result = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SetMultipleBSY");
      if (poll_result < 0) return BLOCK_ERR_TIMEOUT;
      uint8_t status = (uint8_t)poll_result;
      if (!(status & ATA_SR_DRDY) || (status & (ATA_SR_ERR | ATA_SR_DF))) {
          terminal_printf("[ATA %s] Error setting MULTIPLE mode (Status=%#x), disabling feature.\n", dev->device_name, status);
          dev->multiple_sector_count = 0;
      } else {
           terminal_printf("[ATA %s] MULTIPLE mode SET to %u sectors.\n", dev->device_name, dev->multiple_sector_count);
      }
      return BLOCK_ERR_OK;
 }

 /**
  * @brief Sets up the LBA address and sector count registers for a PIO command.
  */
 static void ata_setup_lba(block_device_t *dev, uint64_t lba, size_t count) {
     KERNEL_ASSERT(dev != NULL && count > 0, "Invalid params in ata_setup_lba");
     uint8_t dev_select_base = (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA;
     bool needs_lba48 = dev->lba48_supported && (lba + count -1 >= 0x10000000ULL || count > 256);
     if (needs_lba48) {
         KERNEL_ASSERT(count <= 65536, "LBA48 count exceeds 65536");
         uint16_t sc = (count == 65536) ? 0 : (uint16_t)count;
         outb(dev->io_base + ATA_REG_HDDEVSEL, dev_select_base);
         outb(dev->io_base + ATA_REG_SECCOUNT1, (uint8_t)(sc >> 8));
         outb(dev->io_base + ATA_REG_LBA3, (uint8_t)(lba >> 24));
         outb(dev->io_base + ATA_REG_LBA4, (uint8_t)(lba >> 32));
         outb(dev->io_base + ATA_REG_LBA5, (uint8_t)(lba >> 40));
         outb(dev->io_base + ATA_REG_SECCOUNT0, (uint8_t)(sc & 0xFF));
         outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
         outb(dev->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
         outb(dev->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
     } else {
         KERNEL_ASSERT(lba + count <= 0x10000000ULL, "LBA28 address/count exceeds limit");
         KERNEL_ASSERT(count <= 256, "LBA28 count exceeds 256");
         uint8_t sc = (count == 256) ? 0 : (uint8_t)count;
         uint8_t dev_select = dev_select_base | ((uint8_t)((lba >> 24) & 0x0F));
         outb(dev->io_base + ATA_REG_HDDEVSEL, dev_select);
         outb(dev->io_base + ATA_REG_SECCOUNT0, sc);
         outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
         outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
         outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
     }
 }

 // --- PIO Transfer ---
 
 /**
  * @brief Performs the PIO data transfer for a block of sectors (after IRQ/DRQ is ready).
  */
 static int ata_pio_transfer_block(block_device_t *dev, void *buffer, size_t sectors_in_block, bool write) {
      KERNEL_ASSERT(dev != NULL && buffer != NULL && sectors_in_block > 0 && dev->sector_size > 0, "Invalid params in ata_pio_transfer_block");
      size_t words_per_sector = dev->sector_size / 2;
      uint16_t data_port = dev->io_base + ATA_REG_DATA;
      for (size_t sector = 0; sector < sectors_in_block; sector++) {
          // <<<--- Moved Status/Error Check Inside Loop (Before each sector transfer) --->>>
          uint8_t current_status = inb(dev->io_base + ATA_REG_STATUS);
          // Check BSY before DRQ - BSY must be 0 for DRQ to be valid after initial wait
          if (current_status & ATA_SR_BSY) {
               terminal_printf("[ATA %s IO] Error: BSY set during sector %lu transfer! (Status=%#x)\n", dev->device_name, (unsigned long)sector, current_status);
               return BLOCK_ERR_IO; // Device became busy unexpectedly
          }
          // Now check ERR/DF
          if (current_status & (ATA_SR_ERR | ATA_SR_DF)) {
               terminal_printf("[ATA %s IO] Error: ERR/DF set during sector %lu transfer! (Status=%#x)\n", dev->device_name, (unsigned long)sector, current_status);
               return BLOCK_ERR_DEV_FAULT;
          }
          // Finally, check DRQ - it *must* be set here if BSY/ERR/DF are clear after waiting
          if (!(current_status & ATA_SR_DRQ)) {
               terminal_printf("[ATA %s IO] Error: DRQ not set during sector %lu transfer! (Status=%#x)\n", dev->device_name, (unsigned long)sector, current_status);
               return BLOCK_ERR_IO; // DRQ should be set if command was successful
          }
          // <<<--- End Moved Check --->>>

          uint16_t *buf_words = (uint16_t *)((uint8_t *)buffer + sector * dev->sector_size);
          if (write) {
              for (size_t i = 0; i < words_per_sector; i++) outw(data_port, buf_words[i]);
          } else {
              for (size_t i = 0; i < words_per_sector; i++) buf_words[i] = inw(data_port);
          }
          // After transferring a sector (especially for MULTIPLE mode), a delay or status check might be needed
          // before the next IRQ/DRQ signal for the *next* sector in the block. The current hybrid wait handles the
          // wait *before* the block transfer starts. Let's assume controller handles subsequent sectors okay for now.
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
  * @brief Initializes a block_device_t structure for a given device name.
  */
 int block_device_init(const char *device, block_device_t *dev) {
     if (!device || !dev) return BLOCK_ERR_PARAMS;
     memset(dev, 0, sizeof(block_device_t));
     dev->device_name = device;
     bool primary_channel = true;
     bool is_slave = false;
     if (strcmp(device, "hda") == 0) {}
     else if (strcmp(device, "hdb") == 0) { is_slave = true; }
     else if (strcmp(device, "hdc") == 0) { primary_channel = false; }
     else if (strcmp(device, "hdd") == 0) { primary_channel = false; is_slave = true; }
     else { terminal_printf("[BlockDev Init] Error: Unknown device name '%s'.\n", device); return BLOCK_ERR_PARAMS; }
     dev->io_base = primary_channel ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
     dev->control_base = primary_channel ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
     dev->is_slave = is_slave;
     dev->channel_lock = primary_channel ? &g_ata_primary_lock : &g_ata_secondary_lock;
     terminal_printf("[BlockDev Init] Probing '%s' (IO:%#x, Ctrl:%#x, Slave:%d)...\n", device, dev->io_base, dev->control_base, dev->is_slave);

     uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);

     // --- Debug Print BEFORE ata_identify ---
     terminal_printf("[BlockDev Debug] KBC Status before ata_identify: 0x%x\n", inb(KBC_STATUS_PORT));
     // --- End Debug Print ---
     int ret = ata_identify(dev);
     // --- Debug Print AFTER ata_identify ---
     terminal_printf("[BlockDev Debug] KBC Status after ata_identify: 0x%x\n", inb(KBC_STATUS_PORT));
     // --- End Debug Print ---

     if (ret == BLOCK_ERR_OK) {
         // --- Debug Print BEFORE ata_set_multiple_mode ---
         terminal_printf("[BlockDev Debug] KBC Status before ata_set_multiple_mode: 0x%x\n", inb(KBC_STATUS_PORT));
         // --- End Debug Print ---
         // <<< FIX: Remove redefinition of ret >>>
         ret = ata_set_multiple_mode(dev);
         // --- Debug Print AFTER ata_set_multiple_mode ---
         terminal_printf("[BlockDev Debug] KBC Status after ata_set_multiple_mode: 0x%x\n", inb(KBC_STATUS_PORT));
         // --- End Debug Print ---
         if (ret != BLOCK_ERR_OK) {
              terminal_printf("[BlockDev Init] Warning: Failed to set MULTIPLE mode for '%s' (err %d), continuing without it.\n", device, ret);
              ret = BLOCK_ERR_OK; // Treat as non-fatal
          }
     }
     dev->initialized = (ret == BLOCK_ERR_OK);
     spinlock_release_irqrestore(dev->channel_lock, irq_flags);
     if (!dev->initialized) { terminal_printf("[BlockDev Init] Failed for '%s' during IDENTIFY (err %d).\n", device, ret); return ret; }
     // <<< FIX: Use %llu for uint64_t, %u for uint16_t, %lu for uint32_t >>>
     terminal_printf("[BlockDev Init] OK: '%s' LBA48:%d Sectors:%llu\n",
        device, dev->lba48_supported, dev->total_sectors);
terminal_printf("    -> Mult:%u SectorSize:%lu\n", // Use %u for uint16_t, %lu for uint32_t
        dev->multiple_sector_count, (unsigned long)dev->sector_size);
     return BLOCK_ERR_OK;
 }


 /**
  * @brief Reads or writes sectors to/from a block device using PIO with hybrid IRQ/Polling wait.
  */
  static int block_device_transfer(block_device_t *dev, uint64_t lba, void *buffer, size_t count, bool write) {
     KERNEL_ASSERT(dev && dev->initialized && buffer && count > 0, "Invalid parameters to block_device_transfer");
     KERNEL_ASSERT(dev->sector_size > 0 && (dev->sector_size % 2 == 0), "Invalid sector size");
     KERNEL_ASSERT(lba < dev->total_sectors && count <= dev->total_sectors - lba, "Transfer out of bounds");

     volatile bool* irq_fired_flag;
     volatile uint8_t* last_status_flag;
     volatile uint8_t* last_error_flag;
     bool primary_channel = (dev->io_base == ATA_PRIMARY_IO);
     if (primary_channel) {
         irq_fired_flag = &g_ata_primary_irq_fired;
         last_status_flag = &g_ata_primary_last_status;
         last_error_flag = &g_ata_primary_last_error;
     } else {
         terminal_printf("[ATA %s RW] Error: Secondary channel IRQ handling not implemented.\n", dev->device_name);
         return BLOCK_ERR_UNSUPPORTED;
     }

     uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);
     int final_ret = BLOCK_ERR_OK;
     size_t sectors_remaining = count;
     uint64_t current_lba = lba;
     uint8_t *current_buffer = (uint8_t *)buffer;

     while (sectors_remaining > 0) {
         int current_ret = BLOCK_ERR_OK;
         bool use_lba48 = dev->lba48_supported && (current_lba + sectors_remaining -1 >= 0x10000000ULL);
         size_t max_sectors_per_ata_cmd = use_lba48 ? 65536 : 256;
         bool use_multiple_this_cmd = (dev->multiple_sector_count > 0) && (sectors_remaining >= dev->multiple_sector_count);
         size_t sectors_this_cmd = use_multiple_this_cmd ? dev->multiple_sector_count : 1;
         if (sectors_this_cmd > sectors_remaining) sectors_this_cmd = sectors_remaining;
         if (sectors_this_cmd > max_sectors_per_ata_cmd) sectors_this_cmd = max_sectors_per_ata_cmd;
         use_multiple_this_cmd = use_multiple_this_cmd && (sectors_this_cmd >= dev->multiple_sector_count);

         uint8_t command;
         if (write) command = use_multiple_this_cmd ? (use_lba48 ? ATA_CMD_WRITE_MULTIPLE_EXT:ATA_CMD_WRITE_MULTIPLE) : (use_lba48 ? ATA_CMD_WRITE_PIO_EXT:ATA_CMD_WRITE_PIO);
         else       command = use_multiple_this_cmd ? (use_lba48 ? ATA_CMD_READ_MULTIPLE_EXT :ATA_CMD_READ_MULTIPLE)  : (use_lba48 ? ATA_CMD_READ_PIO_EXT  :ATA_CMD_READ_PIO);
         if (!use_lba48 && (current_lba + sectors_this_cmd > 0x10000000ULL)) { final_ret = BLOCK_ERR_BOUNDS; break; }

         current_ret = ata_select_drive(dev);
         if (current_ret != BLOCK_ERR_OK) { final_ret = current_ret; break; }

         ata_setup_lba(dev, current_lba, sectors_this_cmd);
         *irq_fired_flag = false;
         *last_status_flag = 0;
         *last_error_flag = 0;

         outb(dev->io_base + ATA_REG_COMMAND, command);
         ata_delay_400ns(dev->control_base);

         // --- Hybrid Wait for IRQ or BSY Clear ---
         uint32_t wait_loops = ATA_TIMEOUT_PIO * ATA_IRQ_WAIT_MULTIPLIER;
         bool timed_out = true;
         // <<< FIX: Remove unused variable >>>
         // bool command_done_polling = false;
         uint8_t polled_status = 0;

         while(wait_loops--) {
             if (*irq_fired_flag) { // Check IRQ flag first
                 timed_out = false;
                 break;
             }
             // If no IRQ, poll status register (non-blocking read)
             polled_status = inb(dev->io_base + ATA_REG_STATUS);
             if (!(polled_status & ATA_SR_BSY)) { // Check if BSY is clear
                 // BSY clear means command finished or errored
                 // <<< FIX: Remove assignment to unused variable >>>
                 // command_done_polling = true;
                 timed_out = false; // Consider it not a timeout if BSY cleared
                 break;
             }
             asm volatile ("pause"); // Yield CPU slice slightly
         }
         // Final check for IRQ flag after loop
         if (!timed_out) { /* Already done */ }
         else if (*irq_fired_flag) {
             terminal_printf("[ATA %s RW %s] IRQ detected *after* wait loop finished (Cmd %#x, LBA %llu).\n",
                             dev->device_name, write ? "Write" : "Read", command, current_lba);
             timed_out = false; // Treat as success
         }

         if (timed_out) {
             terminal_printf("[ATA %s RW %s] Timeout waiting for IRQ/BSY Clear (Cmd %#x, LBA %llu)\n",
                             dev->device_name, write ? "Write" : "Read", command, current_lba);
             final_ret = BLOCK_ERR_TIMEOUT;
             *last_status_flag = inb(dev->io_base + ATA_REG_STATUS); // Read status on timeout
              terminal_printf(" -> Last Status before timeout: %#x\n", *last_status_flag);
             break; // Exit outer transfer loop
         }

         // --- Command Completed (via IRQ or BSY Poll) - Check Final Status ---
         uint8_t final_status = *irq_fired_flag ? *last_status_flag : polled_status;
         uint8_t final_error = *irq_fired_flag ? *last_error_flag : ((final_status & ATA_SR_ERR) ? inb(dev->io_base + ATA_REG_ERROR) : 0);

         if (final_status & (ATA_SR_ERR | ATA_SR_DF)) {
             terminal_printf("[ATA %s RW %s] Error/Fault detected (Cmd %#x, LBA %llu, Status=%#x, Error=%#x)\n",
                             dev->device_name, write ? "Write" : "Read", command, current_lba, final_status, final_error);
             final_ret = (final_status & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
             break; // Exit outer loop
         }

         // --- Transfer Data ---
         if (command != ATA_CMD_FLUSH_CACHE && command != ATA_CMD_FLUSH_CACHE_EXT) {
             // Check DRQ bit - MUST be set for successful R/W completion
             if (!(final_status & ATA_SR_DRQ)) {
                 terminal_printf("[ATA %s RW %s] Command done but DRQ not set! (Cmd %#x, LBA %llu, Status=%#x)\n",
                                 dev->device_name, write ? "Write" : "Read", command, current_lba, final_status);
                 final_ret = BLOCK_ERR_IO; break;
             }
             current_ret = ata_pio_transfer_block(dev, current_buffer, sectors_this_cmd, write);
             if (current_ret != BLOCK_ERR_OK) { final_ret = current_ret; break; }
         }

         // Advance state
         sectors_remaining -= sectors_this_cmd;
         current_lba += sectors_this_cmd;
         current_buffer += sectors_this_cmd * dev->sector_size;
     } // End while(sectors_remaining > 0)

     // --- Final Cache Flush --- (Logic remains similar, uses hybrid wait)
     if (write && final_ret == BLOCK_ERR_OK) {
         *irq_fired_flag = false; *last_status_flag = 0; *last_error_flag = 0;
         int sel_ret = ata_select_drive(dev);
         if (sel_ret == BLOCK_ERR_OK) {
             uint8_t flush_cmd = dev->lba48_supported ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE;
             outb(dev->io_base + ATA_REG_COMMAND, flush_cmd);
             ata_delay_400ns(dev->control_base);

             uint32_t wait_loops = ATA_TIMEOUT_PIO * ATA_IRQ_WAIT_MULTIPLIER;
             bool timed_out = true;
             // <<< FIX: Remove unused variable >>>
             // bool cmd_done_poll = false;
             uint8_t poll_stat = 0;
             while(wait_loops--) {
                 if (*irq_fired_flag) { timed_out = false; break; }
                 poll_stat = inb(dev->io_base + ATA_REG_STATUS);
                 if (!(poll_stat & ATA_SR_BSY)) { /*cmd_done_poll = true;*/ timed_out = false; break; } // Removed assignment
                 asm volatile("pause");
             }
             if (!timed_out || *irq_fired_flag) { timed_out = false; } // Final IRQ check

             if (timed_out) {
                 terminal_printf("[ATA %s RW Write] FlushCache timeout.\n", dev->device_name);
                 final_ret = BLOCK_ERR_TIMEOUT;
             } else {
                 uint8_t final_stat = *irq_fired_flag ? *last_status_flag : poll_stat;
                 uint8_t final_err = *irq_fired_flag ? *last_error_flag : ((final_stat & ATA_SR_ERR) ? inb(dev->io_base + ATA_REG_ERROR) : 0);
                 if ((final_stat & ATA_SR_BSY) || (final_stat & (ATA_SR_ERR | ATA_SR_DF))) {
                     terminal_printf("[ATA %s RW Write] FlushCache error/fault/busy (Status=%#x, Error=%#x).\n", dev->device_name, final_stat, final_err);
                     final_ret = (final_stat & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
                 }
             }
         } else {
             terminal_printf("[ATA %s RW Write] Select drive failed before FlushCache (Err %d).\n", dev->device_name, sel_ret);
             if (final_ret == BLOCK_ERR_OK) final_ret = sel_ret;
         }
     }

     // Release Lock and Return
     spinlock_release_irqrestore(dev->channel_lock, irq_flags);
     return final_ret;
 }

 /**
  * @brief Reads sectors from the block device. Public wrapper.
  */
 int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count) {
     return block_device_transfer(dev, lba, buffer, count, false);
 }

 /**
  * @brief Writes sectors to the block device. Public wrapper.
  */
 int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count) {
     return block_device_transfer(dev, lba, (void *)buffer, count, true);
 }

 /**
  * @brief Primary ATA IRQ Handler (IRQ 14 -> Vector 46).
  */
  void ata_primary_irq_handler(isr_frame_t* frame) {
      (void)frame; // Frame not used currently
      uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
      uint8_t error = (status & ATA_SR_ERR) ? inb(ATA_PRIMARY_IO + ATA_REG_ERROR) : 0;
      g_ata_primary_last_status = status;
      g_ata_primary_last_error = error;
      g_ata_primary_irq_fired = true;
      // serial_write('!'); // Minimal debug signal
  }