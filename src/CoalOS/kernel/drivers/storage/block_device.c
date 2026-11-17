/**
 * @file block_device.c
 * @brief ATA PIO driver. One command at a time per channel; the channel lock
 *        yields instead of disabling interrupts, so the timer keeps running.
 */
#define LOG_MODULE "ata"

#include <kernel/drivers/storage/block_device.h>
#include <kernel/drivers/pic.h>
#include <kernel/core/log.h>
#include <kernel/lib/port_io.h>
#include <kernel/lib/string.h>
#include <kernel/process/scheduler.h>

/* Task-file registers (offsets from io_base) */
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0     3
#define ATA_REG_LBA1     4
#define ATA_REG_LBA2     5
#define ATA_REG_DEVSEL   6
#define ATA_REG_COMMAND  7
#define ATA_REG_STATUS   7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ          0x20
#define ATA_CMD_READ_EXT      0x24
#define ATA_CMD_WRITE         0x30
#define ATA_CMD_WRITE_EXT     0x34
#define ATA_CMD_FLUSH         0xE7
#define ATA_CMD_FLUSH_EXT     0xEA
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_DEVCTL_NIEN 0x02 /* written to control_base: no drive interrupts */

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_SPIN_MAX          5000000u
#define ATA_MAX_SECTORS_CMD   256u
#define LBA28_LIMIT           0x10000000ull
#define MAX_SECTOR_SIZE       8192u

static volatile uint8_t g_channel_busy[2];

/* ---- Channel lock ------------------------------------------------------ */

static void channel_lock(uint8_t ch) {
    while (__atomic_exchange_n(&g_channel_busy[ch], 1, __ATOMIC_ACQUIRE)) {
        if (scheduler_is_ready()) yield();
        else asm volatile("pause");
    }
}

static void channel_unlock(uint8_t ch) {
    __atomic_store_n(&g_channel_busy[ch], 0, __ATOMIC_RELEASE);
}

/* ---- Low-level helpers ------------------------------------------------- */

static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    asm volatile("rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline uint8_t alt_status(const block_device_t *d) {
    return inb(d->control_base);
}

/** ~400 ns: the status register is not valid right after a command or drive select. */
static void ata_delay_400ns(const block_device_t *d) {
    for (int i = 0; i < 4; i++) (void)alt_status(d);
}

/** Waits for BSY to clear (and DRQ to set if need_drq). Returns a BLOCK_ERR_* code. */
static int ata_wait(const block_device_t *d, bool need_drq) {
    for (uint32_t i = 0; i < ATA_SPIN_MAX; i++) {
        uint8_t st = alt_status(d);
        if (st & ATA_SR_BSY) continue;
        if (st & ATA_SR_DF)  return BLOCK_ERR_DEV_FAULT;
        if (st & ATA_SR_ERR) return BLOCK_ERR_DEV_ERR;
        if (!need_drq || (st & ATA_SR_DRQ)) return BLOCK_ERR_OK;
    }
    return BLOCK_ERR_TIMEOUT;
}

static void ata_select(const block_device_t *d) {
    outb(d->io_base + ATA_REG_DEVSEL, 0xA0 | (d->is_slave ? 0x10 : 0));
    ata_delay_400ns(d);
}

static void ata_setup_lba(const block_device_t *d, uint64_t lba, uint32_t count, bool ext) {
    uint8_t sel = 0xE0 | (d->is_slave ? 0x10 : 0); /* LBA mode */
    if (ext) {
        outb(d->io_base + ATA_REG_DEVSEL, sel);
        outb(d->io_base + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
        outb(d->io_base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        outb(d->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        outb(d->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 40));
    } else {
        outb(d->io_base + ATA_REG_DEVSEL, sel | (uint8_t)((lba >> 24) & 0x0F));
    }
    outb(d->io_base + ATA_REG_SECCOUNT, (uint8_t)count);
    outb(d->io_base + ATA_REG_LBA0, (uint8_t)lba);
    outb(d->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(d->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
}

/* ---- IDENTIFY ---------------------------------------------------------- */

static int ata_identify(block_device_t *d) {
    uint16_t id[256];

    ata_select(d);
    outb(d->io_base + ATA_REG_SECCOUNT, 0);
    outb(d->io_base + ATA_REG_LBA0, 0);
    outb(d->io_base + ATA_REG_LBA1, 0);
    outb(d->io_base + ATA_REG_LBA2, 0);
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(d);

    uint8_t st = alt_status(d);
    if (st == 0 || st == 0xFF) return BLOCK_ERR_NO_DEV;
    for (uint32_t i = 0; i < ATA_SPIN_MAX && (alt_status(d) & ATA_SR_BSY); i++) { }
    if (inb(d->io_base + ATA_REG_LBA1) || inb(d->io_base + ATA_REG_LBA2)) {
        return BLOCK_ERR_UNSUPPORTED; /* ATAPI/SATA signature, not an ATA disk */
    }
    int r = ata_wait(d, true);
    if (r != BLOCK_ERR_OK) return r;
    insw(d->io_base + ATA_REG_DATA, id, 256);

    if (!(id[49] & (1u << 9))) return BLOCK_ERR_UNSUPPORTED; /* no LBA */

    d->lba48_supported = (id[83] & (1u << 10)) != 0;
    if (d->lba48_supported) {
        d->total_sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                           ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    } else {
        d->total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }
    if (d->total_sectors == 0) return BLOCK_ERR_NO_DEV;

    /* Words 117-118 hold the logical sector size in words when word 106 says
     * it is valid (bit 14 set, bit 15 clear) and larger than 256 words (bit 12). */
    d->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        uint32_t bytes = ((uint32_t)id[117] | ((uint32_t)id[118] << 16)) * 2;
        if (bytes >= 512 && bytes <= MAX_SECTOR_SIZE && (bytes & (bytes - 1)) == 0) {
            d->sector_size = bytes;
        }
    }
    return BLOCK_ERR_OK;
}

/* ---- Data transfer ----------------------------------------------------- */

/** One READ/WRITE command for `count` (1..256) sectors. The channel lock is held. */
static int ata_pio_command(block_device_t *d, uint64_t lba, uint8_t *buf, uint32_t count, bool write) {
    bool ext = d->lba48_supported && (lba + count > LBA28_LIMIT);
    uint8_t cmd = write ? (ext ? ATA_CMD_WRITE_EXT : ATA_CMD_WRITE)
                        : (ext ? ATA_CMD_READ_EXT  : ATA_CMD_READ);
    uint32_t words = d->sector_size / 2;

    ata_select(d);
    int r = ata_wait(d, false);
    if (r != BLOCK_ERR_OK) return r;
    ata_setup_lba(d, lba, count, ext);
    outb(d->io_base + ATA_REG_COMMAND, cmd);
    ata_delay_400ns(d);

    for (uint32_t s = 0; s < count; s++, buf += d->sector_size) {
        r = ata_wait(d, true);
        if (r != BLOCK_ERR_OK) return r;
        if (write) {
            outsw(d->io_base + ATA_REG_DATA, buf, words);
            ata_delay_400ns(d);
        } else {
            insw(d->io_base + ATA_REG_DATA, buf, words);
        }
    }
    return write ? ata_wait(d, false) : BLOCK_ERR_OK;
}

static int block_device_transfer(block_device_t *d, uint64_t lba, uint8_t *buf, size_t count, bool write) {
    if (!d || !d->initialized || !buf || count == 0) return BLOCK_ERR_PARAMS;
    if (lba >= d->total_sectors || count > d->total_sectors - lba) return BLOCK_ERR_BOUNDS;
    if (!d->lba48_supported && lba + count > LBA28_LIMIT) return BLOCK_ERR_BOUNDS;

    int r = BLOCK_ERR_OK;
    channel_lock(d->channel);
    while (count && r == BLOCK_ERR_OK) {
        uint32_t n = count < ATA_MAX_SECTORS_CMD ? (uint32_t)count : ATA_MAX_SECTORS_CMD;
        r = ata_pio_command(d, lba, buf, n, write);
        lba += n;
        buf += (size_t)n * d->sector_size;
        count -= n;
    }
    channel_unlock(d->channel);

    if (r != BLOCK_ERR_OK) {
        LOG_ERROR("%s %s failed at LBA %u (err %d)", d->device_name, write ? "write" : "read",
                  (uint32_t)lba, r);
    }
    return r;
}

/* ---- Public API -------------------------------------------------------- */

int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count) {
    return block_device_transfer(dev, lba, buffer, count, false);
}

int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count) {
    return block_device_transfer(dev, lba, (uint8_t *)buffer, count, true);
}

int block_device_flush(block_device_t *dev) {
    if (!dev || !dev->initialized) return BLOCK_ERR_PARAMS;
    channel_lock(dev->channel);
    ata_select(dev);
    outb(dev->io_base + ATA_REG_COMMAND, dev->lba48_supported ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH);
    ata_delay_400ns(dev);
    int r = ata_wait(dev, false);
    channel_unlock(dev->channel);
    return r;
}

int block_device_init(const char *device, block_device_t *dev) {
    if (!device || !dev || strlen(device) != 3 || device[0] != 'h' || device[1] != 'd' ||
        device[2] < 'a' || device[2] > 'd') {
        return BLOCK_ERR_PARAMS;
    }
    memset(dev, 0, sizeof(*dev));
    dev->device_name  = device;
    dev->channel      = (device[2] >= 'c');
    dev->is_slave     = (device[2] & 1) == 0; /* b, d */
    dev->io_base      = dev->channel ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    dev->control_base = dev->channel ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL;

    channel_lock(dev->channel);
    outb(dev->control_base, ATA_DEVCTL_NIEN);
    int r = ata_identify(dev);
    channel_unlock(dev->channel);

    if (r != BLOCK_ERR_OK) {
        LOG_WARN("%s: no usable drive (err %d)", device, r);
        return r;
    }
    dev->initialized = true;
    LOG_INFO("%s: %u sectors of %u bytes, LBA%d", device, (uint32_t)dev->total_sectors,
             dev->sector_size, dev->lba48_supported ? 48 : 28);
    return BLOCK_ERR_OK;
}

void ata_primary_irq_handler(isr_frame_t *frame) {
    (void)frame;
    (void)inb(ATA_PRIMARY_IO + ATA_REG_STATUS); /* reading status deasserts the IRQ line */
    pic_send_eoi(14);
}
