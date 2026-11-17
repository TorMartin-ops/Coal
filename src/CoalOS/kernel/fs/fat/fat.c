/**
 * @file fat.c
 * @brief FAT16/FAT32 volume: mount, FAT table, cluster chains and allocation.
 *
 * The whole first FAT is cached in memory. Changes are tracked as a dirty
 * sector range and copied into the buffer cache (every FAT copy) by
 * fat_commit(), which the higher layers call after each metadata change.
 * Nothing is forced to disk there: fat_flush() does that on sync, fsync,
 * unmount and from the periodic flusher (fs_init.c), so a burst of creates
 * costs memory copies, not disk writes.
 */
#define LOG_MODULE "fat"
#include <kernel/core/log.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include "fat_priv.h"

typedef struct __attribute__((packed)) {
    uint8_t  jump[3], oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;
    uint16_t spt, heads;
    uint32_t hidden;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags, version;
    uint32_t root_cluster;
} fat_bpb_t;

buffer_t *fat_buf(fat_fs_t *fs, uint32_t lba) { return buffer_get(fs->dev, lba); }

/* ---- FAT table ------------------------------------------------------------ */

static inline bool cluster_ok(const fat_fs_t *fs, uint32_t c) { return c >= 2 && c < fs->nclusters + 2; }

uint32_t fat_cluster_lba(const fat_fs_t *fs, uint32_t c) {
    return cluster_ok(fs, c) ? fs->data_lba + (c - 2) * fs->spc : 0;
}

static uint32_t fat_get(const fat_fs_t *fs, uint32_t c) {
    if (fs->type == 16) return ((const uint16_t *)fs->fat)[c];
    return ((const uint32_t *)fs->fat)[c] & 0x0FFFFFFF;
}

static void fat_set(fat_fs_t *fs, uint32_t c, uint32_t v) {
    uint32_t esz = fs->type == 16 ? 2 : 4;
    if (fs->type == 16) ((uint16_t *)fs->fat)[c] = (uint16_t)v;
    else {
        uint32_t *p = &((uint32_t *)fs->fat)[c];
        *p = (*p & 0xF0000000) | (v & 0x0FFFFFFF);
    }
    uint32_t sec = c * esz / fs->bps;
    if (sec < fs->dirty_lo) fs->dirty_lo = sec;
    if (sec > fs->dirty_hi) fs->dirty_hi = sec;
}

/* Next cluster of the chain, or 0 at its end (or on a corrupt link). */
uint32_t fat_next(const fat_fs_t *fs, uint32_t c) {
    if (!cluster_ok(fs, c)) return 0;
    uint32_t n = fat_get(fs, c);
    return cluster_ok(fs, n) ? n : 0;
}

uint32_t fat_nth(const fat_fs_t *fs, uint32_t first, uint32_t n) {
    uint32_t c = first;
    while (n-- && c) c = fat_next(fs, c);
    return c;
}

void fat_set_eoc(fat_fs_t *fs, uint32_t c) { fat_set(fs, c, 0x0FFFFFFF); }

/* Allocate a zeroed cluster and link it after `prev` (0 = start a new chain). */
int fat_alloc(fat_fs_t *fs, uint32_t prev) {
    uint32_t c = fs->next_free;
    if (!cluster_ok(fs, c)) c = 2;
    uint32_t n = fs->nclusters;
    while (n-- && fat_get(fs, c) != 0) c = (c + 1 < fs->nclusters + 2) ? c + 1 : 2;
    if (fat_get(fs, c) != 0) return -ENOSPC;

    uint32_t lba = fat_cluster_lba(fs, c);
    for (uint32_t s = 0; s < fs->spc; s++) {
        buffer_t *b = fat_buf(fs, lba + s);
        if (!b) return -EIO;
        memset(b->data, 0, fs->bps);
        buffer_mark_dirty(b);
        buffer_release(b);
    }
    fat_set_eoc(fs, c);
    if (prev) fat_set(fs, prev, c);
    fs->next_free = c + 1;
    return (int)c;
}

void fat_free_chain(fat_fs_t *fs, uint32_t c) {
    for (uint32_t guard = fs->nclusters; cluster_ok(fs, c) && guard--; ) {
        uint32_t n = fat_get(fs, c);
        fat_set(fs, c, 0);
        if (c < fs->next_free) fs->next_free = c;
        c = n;
    }
}

/* Copy the dirty FAT range into every FAT copy in the buffer cache. */
int fat_commit(fat_fs_t *fs) {
    int rc = 0;
    for (uint32_t s = fs->dirty_lo; s <= fs->dirty_hi; s++) {
        for (uint32_t k = 0; k < fs->nfats; k++) {
            buffer_t *b = fat_buf(fs, fs->fat_lba + k * fs->fat_sectors + s);
            if (!b) { rc = -EIO; continue; }
            memcpy(b->data, fs->fat + s * fs->bps, fs->bps);
            buffer_mark_dirty(b);
            buffer_release(b);
        }
    }
    fs->dirty_lo = ~0u;
    fs->dirty_hi = 0;
    fs->unsynced = true;
    return rc;
}

/* Commit, then write every dirty buffer to the disk. */
int fat_flush(fat_fs_t *fs) {
    int rc = fat_commit(fs);
    int sync = buffer_cache_sync();
    fs->unsynced = false;
    return rc ? rc : sync;
}

/* ---- Timestamps ----------------------------------------------------------- */

void fat_unix_to_dos(uint32_t t, uint16_t *date, uint16_t *time) {
    uint32_t z = t / 86400 + 719468, secs = t % 86400;
    uint32_t era = z / 146097, doe = z - era * 146097;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    uint32_t y = yoe + era * 400 + (m <= 2);
    if (y < 1980) { y = 1980; m = d = 1; }
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((secs / 3600) << 11) | ((secs / 60 % 60) << 5) | (secs % 60 / 2));
}

uint32_t fat_dos_to_unix(uint16_t date, uint16_t time) {
    if (!date) return 0;
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

/* ---- Mount / unmount ------------------------------------------------------ */

void *fat_mount(const char *dev) {
    fat_fs_t *fs = NULL;
    buffer_t *b = dev ? buffer_get(dev, 0) : NULL;
    if (!b || !b->disk) { LOG_ERROR("cannot read boot sector of %s", dev ? dev : "(null)"); goto fail; }

    fat_bpb_t bpb;
    memcpy(&bpb, b->data, sizeof(bpb));
    bool sig = b->data[510] == 0x55 && b->data[511] == 0xAA;
    uint32_t sector_size = b->disk->blk_dev.sector_size;
    uint64_t disk_sectors = b->disk->blk_dev.total_sectors;
    const char *devname = b->disk->blk_dev.device_name;
    buffer_release(b);
    b = NULL;

    uint32_t spc = bpb.sectors_per_cluster;
    uint32_t total = bpb.total_sectors16 ? bpb.total_sectors16 : bpb.total_sectors32;
    uint32_t fat_size = bpb.fat_size16 ? bpb.fat_size16 : bpb.fat_size32;
    if (!sig || bpb.bytes_per_sector != sector_size || spc == 0 || (spc & (spc - 1)) ||
        bpb.reserved == 0 || bpb.num_fats == 0 || bpb.num_fats > 2 || fat_size == 0 ||
        total == 0 || total > disk_sectors || bpb.bytes_per_sector * spc > 65536u) {
        LOG_ERROR("invalid BPB on %s", dev);
        goto fail;
    }

    fs = kmalloc(sizeof(*fs));
    if (!fs) goto fail;
    memset(fs, 0, sizeof(*fs));
    fs->dev = devname;
    fs->bps = bpb.bytes_per_sector;
    fs->spc = spc;
    fs->cluster_bytes = fs->bps * spc;
    fs->nfats = bpb.num_fats;
    fs->fat_lba = bpb.reserved;
    fs->fat_sectors = fat_size;
    fs->root_lba = fs->fat_lba + fs->nfats * fat_size;
    fs->root_sectors = ((uint32_t)bpb.root_entries * FAT_ENT_SIZE + fs->bps - 1) / fs->bps;
    fs->data_lba = fs->root_lba + fs->root_sectors;
    if (fs->data_lba >= total) goto bad;
    fs->nclusters = (total - fs->data_lba) / spc;

    if (fs->nclusters < 4085) { LOG_ERROR("FAT12 volumes are not supported"); goto fail; }
    fs->type = fs->nclusters < 65525 ? 16 : 32;
    if (fs->type == 16 && (bpb.root_entries == 0 || bpb.fat_size16 == 0)) goto bad;
    if (fs->type == 32) {
        fs->root_cluster = bpb.root_cluster;
        if (bpb.root_entries != 0 || bpb.root_cluster < 2 || bpb.root_cluster >= fs->nclusters + 2) goto bad;
    }
    if ((uint64_t)(fs->nclusters + 2) * (fs->type / 8) > (uint64_t)fat_size * fs->bps) goto bad;

    size_t fat_bytes = (size_t)fat_size * fs->bps;
    fs->fat = kmalloc(fat_bytes);
    if (!fs->fat) goto fail;
    for (uint32_t s = 0; s < fat_size; s++) {
        buffer_t *fb = fat_buf(fs, fs->fat_lba + s);
        if (!fb) goto fail;
        memcpy(fs->fat + (size_t)s * fs->bps, fb->data, fs->bps);
        buffer_release(fb);
    }
    fs->dirty_lo = ~0u;
    fs->next_free = 2;
    LOG_INFO("%s: FAT%u, %u clusters of %u bytes", dev, fs->type, fs->nclusters, fs->cluster_bytes);
    return fs;

bad:
    LOG_ERROR("inconsistent BPB on %s", dev);
fail:
    if (b) buffer_release(b);
    if (fs) kfree(fs->fat);
    kfree(fs);
    return NULL;
}

int fat_unmount(void *ctx) {
    fat_fs_t *fs = ctx;
    fat_lock(fs);
    if (fs->nodes) { fat_unlock(fs); return -EBUSY; }
    int rc = fat_flush(fs);
    fat_unlock(fs);
    kfree(fs->fat);
    kfree(fs);
    return rc;
}

int fat_sync(void *ctx) {
    fat_fs_t *fs = ctx;
    fat_lock(fs);
    int rc = fs->unsynced || fs->dirty_lo <= fs->dirty_hi ? fat_flush(fs) : 0;
    fat_unlock(fs);
    return rc;
}
