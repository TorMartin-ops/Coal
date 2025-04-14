// === START: Replace Group_14/src/block_device.c with this ===
#include "block_device.h"
#include "port_io.h"
#include "terminal.h"
#include "spinlock.h" // Required for locking
#include "string.h"   // Need strcmp, memset
#include "types.h"
#include "fs_errno.h" // May use generic errors

// --- ATA Register Definitions ---
#define ATA_DATA            0
#define ATA_ERROR           1
#define ATA_FEATURES        1
#define ATA_SECCOUNT0       2 // Also SECCOUNT_LBA48_LOW
#define ATA_LBA0            3 // Also LBA_LBA48_LOW_LOW
#define ATA_LBA1            4 // Also LBA_LBA48_LOW_MID
#define ATA_LBA2            5 // Also LBA_LBA48_LOW_HIGH
#define ATA_HDDEVSEL        6
#define ATA_COMMAND         7
#define ATA_STATUS          7
// LBA48 Registers (accessed after writing high bytes to LBA0-2)
#define ATA_SECCOUNT1       ATA_SECCOUNT0 // LBA48_HIGH
#define ATA_LBA3            ATA_LBA0      // LBA48_MID_LOW
#define ATA_LBA4            ATA_LBA1      // LBA48_MID_MID
#define ATA_LBA5            ATA_LBA2      // LBA48_MID_HIGH

// --- ATA Status Flags ---
#define ATA_SR_BSY          0x80 // Busy
#define ATA_SR_DRDY         0x40 // Drive Ready
#define ATA_SR_DF           0x20 // Device Fault
#define ATA_SR_DSC          0x10 // Drive Seek Complete (obsolete)
#define ATA_SR_DRQ          0x08 // Data Request
#define ATA_SR_CORR         0x04 // Corrected data (obsolete)
#define ATA_SR_IDX          0x02 // Index (obsolete)
#define ATA_SR_ERR          0x01 // Error

// --- ATA Commands ---
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_SET_FEATURES    0xEF
#define ATA_CMD_SET_MULTIPLE    0xC6
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_READ_PIO_EXT    0x24 // LBA48 Read PIO
#define ATA_CMD_READ_MULTIPLE   0xC4
#define ATA_CMD_READ_MULTIPLE_EXT 0x29 // LBA48 Read Multiple
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_WRITE_PIO_EXT   0x34 // LBA48 Write PIO
#define ATA_CMD_WRITE_MULTIPLE  0xC5
#define ATA_CMD_WRITE_MULTIPLE_EXT 0x3A // LBA48 Write Multiple
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA // LBA48 Flush Cache

// --- Device Selection Bits ---
#define ATA_DEV_MASTER          0xA0
#define ATA_DEV_SLAVE           0xB0
#define ATA_DEV_LBA             0x40 // Use LBA addressing

// --- Standard I/O Ports ---
#define ATA_PRIMARY_IO          0x1F0
#define ATA_PRIMARY_CTRL        0x3F6
#define ATA_SECONDARY_IO        0x170
#define ATA_SECONDARY_CTRL      0x376

// --- Timeout ---
#define ATA_TIMEOUT_PIO     1000000 // Generous timeout for PIO waits (Corrected constant name)

// --- Global Locks per Channel ---
static spinlock_t g_ata_primary_lock;
static spinlock_t g_ata_secondary_lock;

// --- Wait Functions ---

// Reads status, returns status byte or -1 on timeout/error indication
static int ata_poll_status(uint16_t io_base, uint8_t wait_mask, uint8_t wait_value, uint32_t timeout, const char* context) {
    while (timeout--) {
        uint8_t status = inb(io_base + ATA_STATUS);
        // Check if desired bits match wait_value, ignoring other bits defined in wait_mask
        if ((status & wait_mask) == wait_value) {
             // Before returning success, ensure no error bits are set if we weren't explicitly waiting for them
             if (!(wait_mask & (ATA_SR_ERR | ATA_SR_DF)) && (status & (ATA_SR_ERR | ATA_SR_DF))) {
                 // We were waiting for something non-error (like BSY clear or DRQ set), but an error occurred simultaneously
                 uint8_t err_reg = (status & ATA_SR_ERR) ? inb(io_base + ATA_ERROR) : 0;
                 terminal_printf("[ATA %s @0x%x] Error Poll during wait: Status=0x%x, Error=0x%x\n", context, io_base, status, err_reg);
                 return status; // Return status indicating error
             }
            return status; // Return the status that matched
        }
         // Check for error/fault explicitly if they were NOT part of the wait mask
         if (!(wait_mask & (ATA_SR_ERR | ATA_SR_DF)) && (status & (ATA_SR_ERR | ATA_SR_DF))) {
             uint8_t err_reg = (status & ATA_SR_ERR) ? inb(io_base + ATA_ERROR) : 0;
             terminal_printf("[ATA %s @0x%x] Error Poll detected error: Status=0x%x, Error=0x%x\n", context, io_base, status, err_reg);
             return status; // Return error status immediately
         }
    }
    terminal_printf("[ATA %s @0x%x] Poll timeout (mask=0x%x, val=0x%x). Last status=0x%x\n",
                    context, io_base, wait_mask, wait_value, inb(io_base + ATA_STATUS));
    return -1; // Timeout indicator
}

// 400ns delay - achieved by reading Alternate Status register 4 times
static void ata_delay_400ns(uint16_t ctrl_base) {
    inb(ctrl_base); inb(ctrl_base); inb(ctrl_base); inb(ctrl_base);
}

// Software Reset (Marked as unused for now, but kept)
/*
static int ata_software_reset(uint16_t io_base, uint16_t ctrl_base) {
    terminal_printf("[ATA Ctrl 0x%x] Performing Software Reset...\n", ctrl_base);
    outb(ctrl_base, 0x04); // Set SRST
    ata_delay_400ns(ctrl_base);
    outb(ctrl_base, 0x00); // Clear SRST (and nIEN)
    ata_delay_400ns(ctrl_base);

    // Wait for BSY clear after reset
    int status = ata_poll_status(io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Reset BSY Clear"); // Use ATA_TIMEOUT_PIO
    if (status < 0 || (status & (ATA_SR_ERR | ATA_SR_DF))) {
        terminal_printf("[ATA Ctrl 0x%x] Software Reset failed (status=0x%x).\n", ctrl_base, status);
        return BLOCK_ERR_DEV_FAULT;
    }
    terminal_printf("[ATA Ctrl 0x%x] Software Reset OK (status=0x%x).\n", ctrl_base, status);
    return BLOCK_ERR_OK;
}
*/

// --- Core Operations ---

// Selects drive, handles delays and waits for ready
static int ata_select_drive(block_device_t *dev) {
    // Wait for channel to be free first (BSY and DRQ should be clear)
    int status = ata_poll_status(dev->io_base, ATA_SR_BSY | ATA_SR_DRQ, 0x00, ATA_TIMEOUT_PIO, "Select Wait Idle"); // Use ATA_TIMEOUT_PIO
    if (status < 0) return BLOCK_ERR_TIMEOUT;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return BLOCK_ERR_DEV_FAULT; // Check before selecting

    outb(dev->io_base + ATA_HDDEVSEL, (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA);
    ata_delay_400ns(dev->control_base);

    // Wait for drive to report ready after select (BSY clear, DRDY set)
    status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Select Wait BSY"); // Use ATA_TIMEOUT_PIO
    if (status < 0) return BLOCK_ERR_TIMEOUT;
    status = inb(dev->io_base + ATA_STATUS); // Read final status
    if (!(status & ATA_SR_DRDY) || (status & (ATA_SR_ERR | ATA_SR_DF))) {
        terminal_printf("[ATA Select %s] Drive not ready/error after select (Status=0x%x).\n", dev->device_name, status);
        return BLOCK_ERR_NO_DEV;
    }
    return BLOCK_ERR_OK;
}

// Issues IDENTIFY command and parses results
static int ata_identify(block_device_t *dev) {
    int ret = ata_select_drive(dev);
    if (ret != BLOCK_ERR_OK) return ret;

    // Send IDENTIFY
    outb(dev->io_base + ATA_SECCOUNT0, 0); outb(dev->io_base + ATA_LBA0, 0);
    outb(dev->io_base + ATA_LBA1, 0);    outb(dev->io_base + ATA_LBA2, 0);
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(dev->control_base);

    if (inb(dev->io_base + ATA_STATUS) == 0) return BLOCK_ERR_NO_DEV;

    int status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Identify BSY Clear"); // Use ATA_TIMEOUT_PIO
    if (status < 0) return BLOCK_ERR_TIMEOUT;

    status = ata_poll_status(dev->io_base, ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF, ATA_SR_DRQ, ATA_TIMEOUT_PIO, "Identify DRQ"); // Use ATA_TIMEOUT_PIO
    if (status < 0) return BLOCK_ERR_TIMEOUT;
    if (status & ATA_SR_ERR) return BLOCK_ERR_DEV_ERR;
    if (status & ATA_SR_DF) return BLOCK_ERR_DEV_FAULT;

    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) { identify_data[i] = inw(dev->io_base + ATA_DATA); }

    dev->sector_size = 512;
    if (!(identify_data[49] & (1 << 9))) return BLOCK_ERR_UNSUPPORTED; // No LBA support

    dev->lba48_supported = (identify_data[83] & (1 << 10)) != 0;
    dev->total_sectors = dev->lba48_supported ? *(uint64_t*)&identify_data[100] : *(uint32_t*)&identify_data[60];
    if (dev->total_sectors == 0) return BLOCK_ERR_NO_DEV;

    dev->multiple_sector_count = 0; // Default to disabled
    if (identify_data[88] & 1) { // Check if MULTIPLE supported
        uint16_t mult_count = identify_data[47] & 0xFF;
        // Only enable if count is a reasonable power of 2 (or standard value like 16)
        if (mult_count > 0 && mult_count <= 16) { // Common limit
             dev->multiple_sector_count = mult_count;
             // Optional: check if power of 2: if (mult_count && !(mult_count & (mult_count - 1)))
             terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode (Preferred Count=%u)\n", dev->device_name, dev->multiple_sector_count);
        } else if (mult_count > 0) {
             terminal_printf("[ATA IDENTIFY %s] Supports MULTIPLE mode but count %u > 16, ignoring.\n", dev->device_name, mult_count);
        }
    }
    return BLOCK_ERR_OK;
}

// Set Multiple Mode if supported
static int ata_set_multiple_mode(block_device_t *dev) {
    if (dev->multiple_sector_count == 0 || dev->multiple_sector_count > 16) { // Only proceed if valid count found
        return BLOCK_ERR_OK;
    }
    int ret = ata_select_drive(dev);
    if (ret != BLOCK_ERR_OK) return ret;

    outb(dev->io_base + ATA_SECCOUNT0, dev->multiple_sector_count);
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_SET_MULTIPLE);
    ata_delay_400ns(dev->control_base);

    int status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "SetMultiple BSY"); // Use ATA_TIMEOUT_PIO
    if (status < 0) return BLOCK_ERR_TIMEOUT;
    status = inb(dev->io_base + ATA_STATUS);
    if (!(status & ATA_SR_DRDY) || (status & (ATA_SR_ERR | ATA_SR_DF))) {
        terminal_printf("[ATA %s] Error setting MULTIPLE mode (Status=0x%x), disabling feature.\n", dev->device_name, status);
        dev->multiple_sector_count = 0; // Disable if setting failed
        return BLOCK_ERR_OK; // Continue without error, just feature disabled
    }
    terminal_printf("[ATA %s] MULTIPLE mode SET to %u sectors.\n", dev->device_name, dev->multiple_sector_count);
    return BLOCK_ERR_OK;
}

// Prepare LBA registers for PIO transfer (LBA28 or LBA48)
static void ata_setup_lba(block_device_t *dev, uint64_t lba, size_t count) {
    uint8_t dev_select_base = (dev->is_slave ? ATA_DEV_SLAVE : ATA_DEV_MASTER) | ATA_DEV_LBA;

    if (dev->lba48_supported && (lba + count > 0xFFFFFFF || lba > 0xFFFFFFF)) {
        uint16_t sector_count_reg = (uint16_t)count;
        outb(dev->io_base + ATA_HDDEVSEL, dev_select_base);
        outb(dev->io_base + ATA_SECCOUNT1, (sector_count_reg >> 8) & 0xFF);
        outb(dev->io_base + ATA_LBA3, (uint8_t)((lba >> 24) & 0xFF));
        outb(dev->io_base + ATA_LBA4, (uint8_t)((lba >> 32) & 0xFF));
        outb(dev->io_base + ATA_LBA5, (uint8_t)((lba >> 40) & 0xFF));
        outb(dev->io_base + ATA_SECCOUNT0, sector_count_reg & 0xFF);
        outb(dev->io_base + ATA_LBA0, (uint8_t)((lba >> 0)  & 0xFF));
        outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8)  & 0xFF));
        outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    } else {
        uint8_t sector_count_reg = (count == 256) ? 0 : (uint8_t)count;
        uint8_t dev_select = dev_select_base | (uint8_t)((lba >> 24) & 0x0F);
        outb(dev->io_base + ATA_HDDEVSEL, dev_select);
        outb(dev->io_base + ATA_SECCOUNT0, sector_count_reg);
        outb(dev->io_base + ATA_LBA0, (uint8_t)(lba & 0xFF));
        outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    }
}

// Core PIO data transfer loop for one block/sector (unchanged from previous fix)
static int ata_pio_transfer_block(block_device_t *dev, void *buffer, size_t sectors_in_block, bool write) {
    int ret = BLOCK_ERR_OK;
    size_t words_per_sector = dev->sector_size / 2;
    uint8_t *buf_bytes = (uint8_t *)buffer;

    for (size_t sector = 0; sector < sectors_in_block; sector++) {
        // Wait for DRQ or ERR for this sector/block
        // *** FIX: Use corrected ata_poll_status call ***
        ret = ata_poll_status(dev->io_base, ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF, ATA_SR_DRQ, ATA_TIMEOUT_PIO, "Data DRQ");
        if (ret < 0) return BLOCK_ERR_TIMEOUT; // Timeout
        if (ret & (ATA_SR_ERR | ATA_SR_DF)) { // Check flags from returned status
            terminal_printf("[ATA IO %s] Error/Fault before sector transfer (Status=0x%x)\n",
                            write ? "Write" : "Read", ret);
            return (ret & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
        }

        // Transfer data
        uint16_t *buf_words = (uint16_t *)(buf_bytes + sector * dev->sector_size);
        if (write) {
            for (size_t i = 0; i < words_per_sector; i++) outw(dev->io_base + ATA_DATA, buf_words[i]);
        } else { // Read
            for (size_t i = 0; i < words_per_sector; i++) buf_words[i] = inw(dev->io_base + ATA_DATA);
        }
        ata_delay_400ns(dev->control_base); // Delay AFTER sector transfer
    }

    // After transferring ALL sectors, wait for BSY clear and check final status
    // *** FIX: Use ATA_TIMEOUT_PIO ***
    ret = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Post-Tx BSY Clear");
    if (ret < 0) return BLOCK_ERR_TIMEOUT;
    if (ret & (ATA_SR_ERR | ATA_SR_DF)) {
        return (ret & ATA_SR_ERR) ? BLOCK_ERR_DEV_ERR : BLOCK_ERR_DEV_FAULT;
    }
    return BLOCK_ERR_OK;
}

// --- Public API Init/Read/Write ---

void ata_channels_init(void) {
    spinlock_init(&g_ata_primary_lock);
    spinlock_init(&g_ata_secondary_lock);
}

int block_device_init(const char *device, block_device_t *dev) {
    // ... (Logic remains the same as provided in previous "World Class" version) ...
     if (!device || !dev) return BLOCK_ERR_PARAMS;
    memset(dev, 0, sizeof(block_device_t));
    dev->device_name = device;
    bool primary_channel = true; bool is_slave = false;
    if (strcmp(device, "hda") == 0) { primary_channel = true; is_slave = false; }
    else if (strcmp(device, "hdb") == 0) { primary_channel = true; is_slave = true; }
    else if (strcmp(device, "hdc") == 0) { primary_channel = false; is_slave = false; }
    else if (strcmp(device, "hdd") == 0) { primary_channel = false; is_slave = true; }
    else { return BLOCK_ERR_PARAMS; }
    dev->io_base = primary_channel ? ATA_PRIMARY_IO : ATA_SECONDARY_IO;
    dev->control_base = primary_channel ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    dev->is_slave = is_slave;
    dev->channel_lock = primary_channel ? &g_ata_primary_lock : &g_ata_secondary_lock;
    terminal_printf("[BlockDev Init] Probing '%s' (IO:0x%x, Slave:%d)...\n", device, dev->io_base, dev->is_slave);
    uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);
    int ret = ata_identify(dev);
    if (ret == BLOCK_ERR_OK) { ret = ata_set_multiple_mode(dev); } // Try setting multiple mode
    dev->initialized = (ret == BLOCK_ERR_OK);
    spinlock_release_irqrestore(dev->channel_lock, irq_flags);
    if (ret != BLOCK_ERR_OK && ret != BLOCK_ERR_UNSUPPORTED) { // Allow unsupported multiple mode
         terminal_printf("[BlockDev Init] Failed for '%s' (err %d).\n", device, ret);
         return ret;
    }
    terminal_printf("[BlockDev Init] OK: '%s' LBA48:%d Sectors:%llu Mult:%u\n",
                   device, dev->lba48_supported, dev->total_sectors, dev->multiple_sector_count);
    return BLOCK_ERR_OK;
}

// Generic Read/Write function using MULTIPLE mode where possible
int block_device_transfer(block_device_t *dev, uint64_t lba, void *buffer, size_t count, bool write) {
    if (!dev || !dev->initialized || !buffer || count == 0) return BLOCK_ERR_PARAMS;
    if (lba >= dev->total_sectors || (dev->total_sectors - lba) < count) {
        terminal_printf("[BlockDev RW] Error: LBA %llu + Count %u out of bounds (Total %llu).\n", lba, count, dev->total_sectors);
        return BLOCK_ERR_BOUNDS;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(dev->channel_lock);
    int ret = BLOCK_ERR_OK;
    size_t sectors_remaining = count;
    uint64_t current_lba = lba;
    uint8_t *current_buffer = (uint8_t *)buffer;

    while (sectors_remaining > 0) {
        size_t sectors_this_cmd = sectors_remaining;
        uint8_t command = 0;
        bool use_lba48 = dev->lba48_supported && (current_lba + sectors_this_cmd > 0xFFFFFFF);

        // *** FIX: Correctly use the use_multiple flag ***
        bool use_multiple = (dev->multiple_sector_count > 0) && (sectors_this_cmd >= dev->multiple_sector_count);

        if (use_multiple) {
            sectors_this_cmd = dev->multiple_sector_count;
            // Ensure we don't try to transfer more than remaining, even in multiple mode
            if (sectors_this_cmd > sectors_remaining) sectors_this_cmd = sectors_remaining;

            if (use_lba48) command = write ? ATA_CMD_WRITE_MULTIPLE_EXT : ATA_CMD_READ_MULTIPLE_EXT;
            else command = write ? ATA_CMD_WRITE_MULTIPLE : ATA_CMD_READ_MULTIPLE;
        } else {
            sectors_this_cmd = 1; // Single sector only
            if (use_lba48) command = write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT;
            else command = write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO;
        }
        // *** END FIX ***

        // Clamp command size based on LBA mode limits AND remaining sectors
        size_t max_per_cmd = use_lba48 ? 65536 : 256;
        if (sectors_this_cmd > max_per_cmd) sectors_this_cmd = max_per_cmd;
        // This check might be redundant now due to the check inside the use_multiple block
        if (sectors_this_cmd > sectors_remaining) sectors_this_cmd = sectors_remaining;

        // Check LBA28 address limit if using LBA28 commands
        if (!use_lba48 && (current_lba + sectors_this_cmd > 0x10000000ULL)) {
             terminal_printf("[BlockDev RW] Error: LBA28 command exceeds address limit (LBA %llu, Count %u).\n", current_lba, sectors_this_cmd);
             ret = BLOCK_ERR_BOUNDS; break;
        }

        // 1. Select drive
        ret = ata_select_drive(dev);
        if (ret != BLOCK_ERR_OK) break;

        // 2. Setup LBA and Sector Count
        ata_setup_lba(dev, current_lba, sectors_this_cmd);

        // 3. Issue Command
        outb(dev->io_base + ATA_COMMAND, command);
        ata_delay_400ns(dev->control_base);

        // 4. Transfer Data
        ret = ata_pio_transfer_block(dev, current_buffer, sectors_this_cmd, write);
        if (ret != BLOCK_ERR_OK) {
            terminal_printf("[BlockDev RW %s] Transfer block failed (Cmd 0x%x, LBA %llu, Count %u, Err %d)\n",
                             write ? "Write" : "Read", command, current_lba, sectors_this_cmd, ret);
            break; // Error during transfer
        }

        // Advance
        sectors_remaining -= sectors_this_cmd;
        current_lba += sectors_this_cmd;
        current_buffer += sectors_this_cmd * dev->sector_size;
    }

    // Final cache flush for writes
    if (write && ret == BLOCK_ERR_OK) {
        outb(dev->io_base + ATA_COMMAND, dev->lba48_supported ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);
        ata_delay_400ns(dev->control_base);
        // *** FIX: Use ATA_TIMEOUT_PIO ***
        int flush_status = ata_poll_status(dev->io_base, ATA_SR_BSY, 0x00, ATA_TIMEOUT_PIO, "Flush");
        if (flush_status < 0) ret = BLOCK_ERR_TIMEOUT;
        else if (flush_status & (ATA_SR_ERR | ATA_SR_DF)) ret = BLOCK_ERR_DEV_FAULT;
    }

    spinlock_release_irqrestore(dev->channel_lock, irq_flags);
    return ret;
}

int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count) {
    return block_device_transfer(dev, lba, buffer, count, false);
}

int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count) {
    return block_device_transfer(dev, lba, (void *)buffer, count, true);
}

// === END: Replace Group_14/src/block_device.c with this ===