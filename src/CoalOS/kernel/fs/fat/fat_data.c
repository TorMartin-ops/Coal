/**
 * @file fat_data.c
 * @brief File contents of FAT files: reads and writes go through the page cache.
 *
 * A file's pages are keyed by its first cluster, which is unique while the file
 * exists and stays the same across open/close and rename. Bytes of a page
 * beyond the file size are always zero, so growing a file needs no extra zeroing.
 * Misses read a window of pages with one disk command (the window doubles while
 * the access stays sequential); dirty pages are written back in runs by
 * fat_writeback() before the FAT and directory sectors that describe them.
 * Everything here runs under the volume lock.
 */
#define LOG_MODULE "fat"
#include <kernel/core/log.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>
#include "fat_priv.h"

#define PG        PC_PAGE_SIZE
#define RA_INIT   4u
#define RA_MAX    32u               /* pages: 128 KiB */
#define IO_SEGS   (RA_MAX + 8)

static inline uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

static fat_chain_t *chain_of(fat_node_t *n) {
    fat_chain_t *c = &n->chain;
    if (c->first != n->first) { c->first = n->first; c->idx = 0; c->cl = 0; }
    return c;
}

/* ---- Disk I/O -------------------------------------------------------------- */

/* Move bytes [i0 * PG, end) of a file between disk and the pages pg[] (pg[0] is page i0), one disk command
 * per run of contiguous sectors. `end` is a multiple of the sector size. */
static int pages_io(fat_fs_t *fs, fat_chain_t *ch, uint32_t i0, pc_page_t *const *pg, uint32_t end, bool write) {
    blk_seg_t seg[IO_SEGS];
    unsigned ns = 0;
    uint32_t run_lba = 0, next_lba = 0;
    int rc = 0;

    for (uint32_t b = i0 * PG; b < end && rc == 0; ) {
        uint32_t cl = fat_chain_get(ch, fs, b / fs->cluster_bytes);
        if (!cl) return -EIO;
        uint32_t in_cl = b % fs->cluster_bytes;
        uint32_t len = MIN(MIN(fs->cluster_bytes - in_cl, PG - b % PG), end - b);
        uint32_t lba = fat_cluster_lba(fs, cl) + in_cl / fs->bps;
        uint8_t *mem = pg[b / PG - i0]->data + b % PG;

        if (ns && (lba != next_lba || ns == IO_SEGS)) {
            rc = disk_rw_sg(fs->disk, run_lba, seg, ns, write);
            ns = 0;
        }
        if (!ns) run_lba = lba;
        if (ns && (uint8_t *)seg[ns - 1].buf + seg[ns - 1].sectors * fs->bps == mem) seg[ns - 1].sectors += len / fs->bps;
        else seg[ns++] = (blk_seg_t){ mem, len / fs->bps };
        next_lba = lba + len / fs->bps;
        b += len;
    }
    if (ns && rc == 0) rc = disk_rw_sg(fs->disk, run_lba, seg, ns, write);
    return rc ? -EIO : 0;
}

/* Write all dirty pages of the volume. Pages of one file that are neighbours go out together. */
int fat_writeback(fat_fs_t *fs) {
    fat_chain_t ch = { 0, 0, 0 };
    pc_page_t *pg[RA_MAX], *p;

    while ((p = pc_first_dirty(fs))) {
        unsigned np = 1;
        pg[0] = p;
        while (np < RA_MAX && (pg[np] = pc_find(fs, p->key, p->idx + np))) {
            if (!pg[np]->dirty) { pc_put(pg[np]); break; }
            np++;
        }
        if (ch.first != p->key) ch = (fat_chain_t){ p->key, 0, 0 };

        uint32_t start = p->idx * PG, end = start + np * PG;     /* only clusters the file owns are written */
        while (end > start && !fat_chain_get(&ch, fs, (end - 1) / fs->cluster_bytes))
            end = (end - 1) / fs->cluster_bytes * fs->cluster_bytes;
        int rc = pages_io(fs, &ch, p->idx, pg, end, true);
        for (unsigned i = 0; i < np; i++) {
            if (rc == 0) pc_mark_clean(pg[i]);
            pc_put(pg[i]);
        }
        if (rc) return rc;
    }
    return 0;
}

/* ---- Pages ------------------------------------------------------------------- */

/* A new page; when memory is short, write out and give back what can go. */
static pc_page_t *page_new(fat_fs_t *fs, uint32_t key, uint32_t idx) {
    pc_page_t *p = pc_add(fs, key, idx);
    if (!p) {
        fat_writeback(fs);
        pc_shrink(RA_MAX);
        p = pc_add(fs, key, idx);
    }
    return p;
}

/* Page `idx` of a file that is not cached: read it and, if the access looks sequential, the pages after it. */
static pc_page_t *page_read(fat_node_t *n, uint32_t idx) {
    fat_fs_t *fs = n->fs;
    uint32_t last = (n->size + PG - 1) / PG;
    uint32_t win = idx == n->ra_next ? MIN(MAX(n->ra_win * 2, RA_INIT), RA_MAX) : 1;
    pc_page_t *pg[RA_MAX];
    unsigned np = 0;

    n->ra_win = win;
    for (; np < win && idx + np < last; np++) {
        if (np) {
            pc_page_t *q = pc_find(fs, n->first, idx + np);
            if (q) { pc_put(q); break; }
        }
        pg[np] = np ? pc_add(fs, n->first, idx + np) : page_new(fs, n->first, idx);
        if (!pg[np]) break;
    }
    if (!np) return NULL;

    uint32_t end = MIN((idx + np) * PG, round_up(n->size, fs->bps));
    int rc = pages_io(fs, chain_of(n), idx, pg, end, false);
    if (rc == 0 && (idx + np) * PG > n->size) {              /* last page: nothing but zeros past the end */
        uint32_t in = n->size - (idx + np - 1) * PG;
        memset(pg[np - 1]->data + in, 0, PG - in);
    }
    for (unsigned i = 1; i < np; i++) pc_put(pg[i]);
    if (rc) {
        pc_put(pg[0]);
        pc_drop(fs, n->first, idx, idx + np);
        return NULL;
    }
    return pg[0];
}

/* Page for writing bytes [in, in + chunk) of page idx. Pages that the write covers need no disk read. */
static pc_page_t *page_for_write(fat_node_t *n, uint32_t idx, uint32_t in, uint32_t chunk) {
    pc_page_t *p = pc_find(n->fs, n->first, idx);
    if (p) return p;
    bool covers = in == 0 && (chunk == PG || idx * PG + chunk >= n->size);
    if (idx * PG < n->size && !covers) return page_read(n, idx);
    p = page_new(n->fs, n->first, idx);
    if (p && chunk != PG) memset(p->data, 0, PG);
    return p;
}

/* Make sure the chain has a cluster `last` (0-based), appending as needed. */
static int node_reserve(fat_node_t *n, uint32_t last) {
    fat_fs_t *fs = n->fs;
    if (!n->first) {
        int c = fat_alloc(fs, 0, false);
        if (c < 0) return c;
        n->first = (uint32_t)c;
    }
    fat_chain_t *ch = chain_of(n);
    if (!ch->cl) ch->cl = ch->first;
    if (last < ch->idx) { ch->idx = 0; ch->cl = ch->first; }
    while (ch->idx < last) {
        uint32_t nx = fat_next(fs, ch->cl);
        if (!nx) {
            int c = fat_alloc(fs, ch->cl, false);
            if (c < 0) return c;
            nx = (uint32_t)c;
        }
        ch->cl = nx;
        ch->idx++;
    }
    return 0;
}

/* Move bytes between buf and the file at pos (buf == NULL writes zeros). */
ssize_t fat_node_rw(fat_node_t *n, uint32_t pos, uint8_t *buf, uint32_t len, bool write) {
    fat_fs_t *fs = n->fs;
    uint32_t done = 0;
    int err = 0;

    while (done < len) {
        uint32_t off = pos + done, idx = off / PG, in = off % PG, chunk = MIN(PG - in, len - done);
        pc_page_t *p;
        if (write) {
            err = node_reserve(n, (off + chunk - 1) / fs->cluster_bytes);
            if (err) break;
            p = page_for_write(n, idx, in, chunk);
        } else {
            p = pc_find(fs, n->first, idx);
            if (!p) p = page_read(n, idx);
        }
        if (!p) { err = -EIO; break; }
        if (!write) memcpy(buf + done, p->data + in, chunk);
        else {
            if (buf) memcpy(p->data + in, buf + done, chunk); else memset(p->data + in, 0, chunk);
            pc_mark_dirty(p);
        }
        pc_put(p);
        n->ra_next = idx + 1;
        done += chunk;
    }
    if (write && pc_too_dirty()) {
        int rc = fat_writeback(fs);
        if (rc && !err) err = rc;
    }
    return done ? (ssize_t)done : err;
}

/* ---- Dropping pages --------------------------------------------------------- */

/* The clusters of a file of `size` bytes are about to be freed: its pages are dead. */
void fat_forget(fat_fs_t *fs, uint32_t first, uint32_t size) {
    pc_drop(fs, first, 0, (size + PG - 1) / PG);
}

/* Shrinking to `len`: zero the rest of the last page and drop the pages behind it. */
void fat_node_trim(fat_node_t *n, uint32_t len) {
    uint32_t keep = (len + PG - 1) / PG, tail = keep * PG - len;
    if (tail) fat_node_rw(n, len, NULL, MIN(tail, n->size - len), true);
    pc_drop(n->fs, n->first, keep, (n->size + PG - 1) / PG);
}
