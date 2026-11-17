/**
 * @file disk.c
 * @brief Logical disk layer: wraps the block device and parses the MBR.
 */
#include <kernel/drivers/storage/disk.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

typedef struct __attribute__((packed)) {
    uint8_t  boot_indicator;
    uint8_t  chs_start[3];
    uint8_t  type;
    uint8_t  chs_end[3];
    uint32_t start_lba;
    uint32_t total_sectors;
} mbr_entry_t;

#define MBR_TABLE_OFFSET 446
#define MBR_SIGNATURE    0xAA55

/** Fills disk->partitions from sector 0; leaves them invalid when there is no MBR. */
static void parse_mbr(disk_t *disk) {
    uint8_t *sector = kmalloc(disk->blk_dev.sector_size);
    if (!sector) return;

    if (disk_read_raw_sectors(disk, 0, sector, 1) == BLOCK_ERR_OK &&
        (uint16_t)(sector[510] | (sector[511] << 8)) == MBR_SIGNATURE) {
        const mbr_entry_t *entries = (const mbr_entry_t *)(sector + MBR_TABLE_OFFSET);
        uint64_t disk_sectors = disk->blk_dev.total_sectors;
        disk->has_mbr = true;
        for (int i = 0; i < MAX_PARTITIONS_PER_DISK; i++) {
            mbr_entry_t e;
            memcpy(&e, &entries[i], sizeof(e)); /* the table is not naturally aligned */
            partition_t *p = &disk->partitions[i];
            p->is_valid = e.type != 0 && e.total_sectors != 0 &&
                          e.start_lba < disk_sectors &&
                          e.total_sectors <= disk_sectors - e.start_lba;
            p->type = e.type;
            p->start_lba = e.start_lba;
            p->total_sectors = e.total_sectors;
        }
    }
    kfree(sector);
}

int disk_init(disk_t *disk, const char *device_name) {
    if (!disk || !device_name) return BLOCK_ERR_PARAMS;

    memset(disk, 0, sizeof(*disk));
    int r = block_device_init(device_name, &disk->blk_dev);
    if (r != BLOCK_ERR_OK) return r;

    disk->initialized = true;
    parse_mbr(disk);
    return BLOCK_ERR_OK;
}

int disk_read_raw_sectors(disk_t *disk, uint64_t lba, void *buffer, size_t count) {
    if (!disk || !disk->initialized) return BLOCK_ERR_PARAMS;
    return block_device_read(&disk->blk_dev, lba, buffer, count);
}

int disk_write_raw_sectors(disk_t *disk, uint64_t lba, const void *buffer, size_t count) {
    if (!disk || !disk->initialized) return BLOCK_ERR_PARAMS;
    return block_device_write(&disk->blk_dev, lba, buffer, count);
}

int disk_rw_sg(disk_t *disk, uint64_t lba, const blk_seg_t *segs, unsigned nseg, bool write) {
    if (!disk || !disk->initialized) return BLOCK_ERR_PARAMS;
    return block_device_rw(&disk->blk_dev, lba, segs, nseg, write);
}

int disk_flush(disk_t *disk) {
    if (!disk || !disk->initialized) return BLOCK_ERR_PARAMS;
    return block_device_flush(&disk->blk_dev);
}
