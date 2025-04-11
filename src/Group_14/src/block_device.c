#include "block_device.h"
#include "port_io.h"
#include "terminal.h"
#include "kmalloc.h"
#include "string.h" // Need strcmp
#include "types.h"

// --- ATA Register Definitions --- (Same as before)
#define ATA_DATA            0
#define ATA_ERROR           1
#define ATA_FEATURES        1
#define ATA_SECCOUNT0       2
#define ATA_LBA0            3
#define ATA_LBA1            4
#define ATA_LBA2            5
#define ATA_HDDEVSEL        6
#define ATA_COMMAND         7
#define ATA_STATUS          7

// --- ATA Status Flags --- (Same as before)
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DSC          0x10
#define ATA_SR_DRQ          0x08
#define ATA_SR_CORR         0x04
#define ATA_SR_IDX          0x02
#define ATA_SR_ERR          0x01

// --- ATA Commands --- (Same as before)
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH_CACHE 0xE7 // Optional

// --- Device Selection ---
// Base values for drive selection byte (HDDEVSEL)
#define ATA_SELECT_MASTER_DRIVE 0xA0 // Sets bit 7=1, bit 5=1, bit 4=0 (Master)
#define ATA_SELECT_SLAVE_DRIVE  0xB0 // Sets bit 7=1, bit 5=1, bit 4=1 (Slave)
// Flag to enable LBA mode (combined with drive select)
#define ATA_LBA_MODE_FLAG       0x40 // Sets bit 6=1

// --- Standard I/O Ports ---
#define ATA_PRIMARY_IO_BASE     0x1F0
#define ATA_PRIMARY_CTRL_BASE   0x3F6
#define ATA_SECONDARY_IO_BASE   0x170
#define ATA_SECONDARY_CTRL_BASE 0x376

// --- Timeout --- (Same as before)
#define ATA_TIMEOUT         100000

// --- Internal Helper Functions --- (ata_wait_busy, ata_wait_drdy, ata_wait_drq, ata_status_poll same as before)
static int ata_wait_busy(uint16_t io_base) { /* ... same ... */
    uint32_t t = ATA_TIMEOUT; while(t--) { if (!(inb(io_base + ATA_STATUS) & ATA_SR_BSY)) return 0; }
    terminal_write("[ATA] Wait busy timeout.\n"); return -1;
}
static int ata_wait_drdy(uint16_t io_base) { /* ... same ... */
    uint32_t t = ATA_TIMEOUT; while(t--) { uint8_t s = inb(io_base+ATA_STATUS); if(!(s&ATA_SR_BSY)&&(s&ATA_SR_DRDY)) return 0; }
    terminal_write("[ATA] Wait DRDY timeout.\n"); return -1;
}
static int ata_wait_drq(uint16_t io_base) { /* ... same ... */
    uint32_t t = ATA_TIMEOUT; while(t--) { uint8_t s = inb(io_base+ATA_STATUS); if(s&ATA_SR_DRQ) return 0; if(s&ATA_SR_ERR){ terminal_write("[ATA] ERR set wait DRQ.\n"); return -2;} }
    terminal_write("[ATA] Wait DRQ timeout.\n"); return -1;
}
static void ata_status_poll(uint16_t io_base) { /* ... same ... */
    inb(io_base + ATA_STATUS); inb(io_base + ATA_STATUS); inb(io_base + ATA_STATUS); inb(io_base + ATA_STATUS);
}

/* Select drive (master/slave) on the specified channel */
static void ata_select_drive(uint16_t io_base, bool is_slave) {
    // Wait for BSY and DRQ to clear before selecting
    while (inb(io_base + ATA_STATUS) & (ATA_SR_BSY | ATA_SR_DRQ));

    uint8_t drive_select_byte = is_slave ? ATA_SELECT_SLAVE_DRIVE : ATA_SELECT_MASTER_DRIVE;
    outb(io_base + ATA_HDDEVSEL, drive_select_byte);

    // Wait ~400ns after drive select (achieved by polling status)
    ata_status_poll(io_base);
}


/* Identify the ATA device (now selects drive based on dev->is_slave) */
static int ata_identify(block_device_t *dev) {
    // Select the correct drive (Master or Slave)
    ata_select_drive(dev->io_base, dev->is_slave);

    // Reset sector counts and LBA registers
    outb(dev->io_base + ATA_SECCOUNT0, 0);
    outb(dev->io_base + ATA_LBA0, 0);
    outb(dev->io_base + ATA_LBA1, 0);
    outb(dev->io_base + ATA_LBA2, 0);

    // Issue IDENTIFY command
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_IDENTIFY);

    // Check status
    uint8_t status = inb(dev->io_base + ATA_STATUS);
    if (status == 0) { /* ... no device ... */ terminal_write("[ATA] IDENTIFY fail: No device/status=0.\n"); return -1; }

    // Wait for BSY to clear
    if (ata_wait_busy(dev->io_base) != 0) return -1;

    // If LBA Mid/High are non-zero now, it's not ATA (e.g., ATAPI)
    // Skip this check for simplicity, as ATAPI might still respond.

    // Wait for DRQ or ERR
    while(1) {
        status = inb(dev->io_base + ATA_STATUS);
        if (status & ATA_SR_ERR) { /* ... error ... */ terminal_write("[ATA] IDENTIFY fail: ERR set.\n"); return -2; }
        if (status & ATA_SR_DRQ) { break; } // Ready
        // Add small delay or check BSY again?
    }

    // Read IDENTIFY data
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(dev->io_base + ATA_DATA);
    }

    // Extract LBA28 sector count
    dev->total_sectors = ((uint32_t)identify_data[61] << 16) | identify_data[60];
    dev->sector_size = 512; // Assume standard size

    return 0; // Success
}

// --- Public API Functions ---

/* Initializes the block device */
int block_device_init(const char *device, block_device_t *dev) {
    if (!device || !dev) {
        terminal_write("[BlockDev] Init: Invalid parameters.\n");
        return -1;
    }
    memset(dev, 0, sizeof(block_device_t)); // Clear struct first
    dev->device_name = device; // Store name (careful about lifetime)

    // --- Determine Ports and Drive based on name ---
    bool primary_channel = true;
    bool is_slave = false;

    if (strcmp(device, "hda") == 0) {
        primary_channel = true; is_slave = false;
    } else if (strcmp(device, "hdb") == 0) {
        primary_channel = true; is_slave = true;
    } else if (strcmp(device, "hdc") == 0) {
        primary_channel = false; is_slave = false;
    } else if (strcmp(device, "hdd") == 0) {
        primary_channel = false; is_slave = true;
    } else {
        terminal_printf("[BlockDev] Init: Unrecognized device name '%s'. Assuming hda.\n", device);
        // Default to primary master if name is unknown? Or return error?
        // return -1; // Option: return error for unknown device
    }

    dev->io_base = primary_channel ? ATA_PRIMARY_IO_BASE : ATA_SECONDARY_IO_BASE;
    dev->control_base = primary_channel ? ATA_PRIMARY_CTRL_BASE : ATA_SECONDARY_CTRL_BASE;
    dev->is_slave = is_slave; // Store slave flag in struct

    terminal_printf("[BlockDev] Probing device '%s' (IO: 0x%x, Slave: %d)...\n",
                    device, dev->io_base, dev->is_slave);

    // Probe the selected device
    if (ata_identify(dev) != 0) {
        terminal_printf("[BlockDev] Init: IDENTIFY failed for '%s'.\n", device);
        return -1;
    }

    terminal_printf("[BlockDev] Initialized device '%s'. Sectors: %u, Sector Size: %u\n",
                   device, dev->total_sectors, dev->sector_size);
    return 0; // Success
}

/* Reads 'count' sectors from the device starting at LBA */
int block_device_read(block_device_t *dev, uint32_t lba, void *buffer, size_t count) {
    if (!dev || !buffer || count == 0 || count > 256) { /* ... param check ... */ return -1; }
    if (dev->total_sectors == 0) { terminal_write("[BlockDev] Read: Device not initialized or size 0.\n"); return -1;}
    if (lba + count > dev->total_sectors) { /* ... bounds check ... */ return -1; }

    // Wait for drive ready
    if (ata_wait_drdy(dev->io_base) != 0) return -1;

    // Select drive and set LBA mode
    uint8_t drive_select_byte = dev->is_slave ? ATA_SELECT_SLAVE_DRIVE : ATA_SELECT_MASTER_DRIVE;
    outb(dev->io_base + ATA_HDDEVSEL, drive_select_byte | ATA_LBA_MODE_FLAG | ((lba >> 24) & 0x0F));

    // Set sector count and LBA address (LBA28)
    uint8_t sector_count = (count == 256) ? 0 : (uint8_t)count;
    outb(dev->io_base + ATA_FEATURES, 0);
    outb(dev->io_base + ATA_SECCOUNT0, sector_count);
    outb(dev->io_base + ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    // Issue READ command
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_READ_PIO);

    // Read data
    uint8_t *buf_ptr = (uint8_t *)buffer;
    for (size_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq(dev->io_base) != 0) return -1;
        uint16_t *buf16 = (uint16_t *)(buf_ptr + sector * dev->sector_size);
        for (uint32_t i = 0; i < (dev->sector_size / 2); i++) { 
            buf16[i] = inw(dev->io_base + ATA_DATA);
        }
        ata_status_poll(dev->io_base);
    }
    return 0; // Success
}

/* Writes 'count' sectors from buffer to the device starting at LBA */
int block_device_write(block_device_t *dev, uint32_t lba, const void *buffer, size_t count) {
    if (!dev || !buffer || count == 0 || count > 256) { /* ... param check ... */ return -1; }
     if (dev->total_sectors == 0) { terminal_write("[BlockDev] Write: Device not initialized or size 0.\n"); return -1;}
    if (lba + count > dev->total_sectors) { /* ... bounds check ... */ return -1; }

    // Wait for drive ready
    if (ata_wait_drdy(dev->io_base) != 0) return -1;

    // Select drive and set LBA mode
    uint8_t drive_select_byte = dev->is_slave ? ATA_SELECT_SLAVE_DRIVE : ATA_SELECT_MASTER_DRIVE;
    outb(dev->io_base + ATA_HDDEVSEL, drive_select_byte | ATA_LBA_MODE_FLAG | ((lba >> 24) & 0x0F));

    // Set sector count and LBA address
    uint8_t sector_count = (count == 256) ? 0 : (uint8_t)count;
    outb(dev->io_base + ATA_FEATURES, 0);
    outb(dev->io_base + ATA_SECCOUNT0, sector_count);
    outb(dev->io_base + ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    // Issue WRITE command
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_WRITE_PIO);

    // Write data
    const uint8_t *buf_ptr = (const uint8_t *)buffer;
    for (size_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq(dev->io_base) != 0) return -1;
        const uint16_t *buf16 = (const uint16_t *)(buf_ptr + sector * dev->sector_size);
        for (uint32_t i = 0; i < (dev->sector_size / 2); i++) {
            outw(dev->io_base + ATA_DATA, buf16[i]);
        }
        ata_status_poll(dev->io_base);
    }

    // Optional: Flush cache
    // outb(dev->io_base + ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    // ata_wait_busy(dev->io_base); // Wait for flush to complete

    return 0; // Success
}


// Need to add the is_slave flag to the block_device_t struct definition
// in block_device.h