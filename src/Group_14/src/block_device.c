#include "block_device.h"
#include "port_io.h"      // Provides inb(), outb(), inw(), outw()
#include "terminal.h"
#include "kmalloc.h"
#include "string.h"
#include "types.h"

// ATA register offsets relative to the I/O base.
#define ATA_DATA            0       // Data register.
#define ATA_ERROR           1       // Error register (read).
#define ATA_FEATURES        1       // Features (write).
#define ATA_SECCOUNT0       2       // Sector count.
#define ATA_LBA0            3       // LBA low.
#define ATA_LBA1            4       // LBA mid.
#define ATA_LBA2            5       // LBA high.
#define ATA_HDDEVSEL        6       // Drive/Head register.
#define ATA_COMMAND         7       // Command register (write).
#define ATA_STATUS          7       // Status register (read).

// ATA status flags.
#define ATA_SR_BSY          0x80    // Busy.
#define ATA_SR_DRDY         0x40    // Drive ready.
#define ATA_SR_DRQ          0x08    // Data request ready.

// ATA commands.
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30

// Timeout constant.
#define ATA_TIMEOUT         100000

/* Internal helper: Wait until the device is not busy. */
static int ata_wait_busy(uint16_t io_base) {
    uint32_t timeout = ATA_TIMEOUT;
    while (timeout--) {
        uint8_t status = inb(io_base + ATA_STATUS);
        if (!(status & ATA_SR_BSY))
            return 0;
    }
    terminal_write("[ATA] Wait busy timeout.\n");
    return -1;
}

/* Internal helper: Wait for the DRQ flag to be set. */
static int ata_wait_drq(uint16_t io_base) {
    uint32_t timeout = ATA_TIMEOUT;
    while (timeout--) {
        uint8_t status = inb(io_base + ATA_STATUS);
        if (status & ATA_SR_DRQ)
            return 0;
    }
    terminal_write("[ATA] Wait DRQ timeout.\n");
    return -1;
}

/* Internal helper: Identify the ATA device.
 * Fills dev->total_sectors based on IDENTIFY data.
 */
static int ata_identify(block_device_t *dev) {
    // Select master device.
    outb(dev->io_base + ATA_HDDEVSEL, 0xA0);
    // Issue IDENTIFY command.
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_IDENTIFY);
    if (ata_wait_busy(dev->io_base) != 0)
        return -1;
    if (ata_wait_drq(dev->io_base) != 0)
        return -1;
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(dev->io_base + ATA_DATA);
    }
    dev->total_sectors = ((uint32_t)identify_data[61] << 16) | identify_data[60];
    return 0;
}

/* Initializes the block device. */
int block_device_init(const char *device, block_device_t *dev) {
    if (!device || !dev) {
        terminal_write("[BlockDev] Init: Invalid parameters.\n");
        return -1;
    }
    dev->device_name = device;
    dev->io_base = 0x1F0;
    dev->control_base = 0x3F6;
    dev->sector_size = 512;  // Standard ATA sector size.
    if (ata_identify(dev) != 0) {
        terminal_write("[BlockDev] Init: IDENTIFY failed.\n");
        return -1;
    }
    terminal_write("[BlockDev] Initialized device ");
    terminal_write(device);
    terminal_write(".\n");
    return 0;
}

/* Reads 'count' sectors from the device starting at LBA into buffer. */
int block_device_read(block_device_t *dev, uint32_t lba, void *buffer, size_t count) {
    if (!dev || !buffer || count == 0) {
        terminal_write("[BlockDev] Read: Invalid parameters.\n");
        return -1;
    }
    if (ata_wait_busy(dev->io_base) != 0)
        return -1;
    // Set up LBA addressing (LBA28 mode).
    outb(dev->io_base + ATA_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(dev->io_base + ATA_SECCOUNT0, (uint8_t)count);
    outb(dev->io_base + ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    // Issue READ PIO command.
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_READ_PIO);
    for (size_t sector = 0; sector < count; sector++) {
        if (ata_wait_busy(dev->io_base) != 0)
            return -1;
        if (ata_wait_drq(dev->io_base) != 0)
            return -1;
        uint16_t *buf16 = (uint16_t *)((uint8_t *)buffer + sector * dev->sector_size);
        for (int i = 0; i < 256; i++) {
            buf16[i] = inw(dev->io_base + ATA_DATA);
        }
    }
    return 0;
}

/* Writes 'count' sectors from buffer to the device starting at LBA. */
int block_device_write(block_device_t *dev, uint32_t lba, const void *buffer, size_t count) {
    if (!dev || !buffer || count == 0) {
        terminal_write("[BlockDev] Write: Invalid parameters.\n");
        return -1;
    }
    if (ata_wait_busy(dev->io_base) != 0)
        return -1;
    outb(dev->io_base + ATA_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(dev->io_base + ATA_SECCOUNT0, (uint8_t)count);
    outb(dev->io_base + ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    // Issue WRITE PIO command.
    outb(dev->io_base + ATA_COMMAND, ATA_CMD_WRITE_PIO);
    for (size_t sector = 0; sector < count; sector++) {
        if (ata_wait_busy(dev->io_base) != 0)
            return -1;
        if (ata_wait_drq(dev->io_base) != 0)
            return -1;
        const uint16_t *buf16 = (const uint16_t *)((const uint8_t *)buffer + sector * dev->sector_size);
        for (int i = 0; i < 256; i++) {
            outw(dev->io_base + ATA_DATA, buf16[i]);
        }
    }
    // Optionally, flush cache here if necessary.
    return 0;
}
