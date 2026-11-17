/**
 * @file fat_dir.c
 * @brief FAT directories: slot access, long file names, lookup, create, remove.
 *
 * fat_dir_next() is the one directory iterator; lookup, readdir, emptiness
 * checks and short-name uniqueness all build on it.
 */
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>
#include "fat_priv.h"

#define MAX_LFN_SLOTS 20
#define MAX_LFN_CHARS (MAX_LFN_SLOTS * 13)
#define MAX_DIR_SLOTS 65536u

/* Byte offsets of the 13 UCS-2 characters inside a long-name slot. */
static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static uint8_t lfn_csum(const uint8_t *sfn) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
}

/* ---- Slot access ---------------------------------------------------------- */

void fat_dir_open(fat_dir_t *d, fat_fs_t *fs, uint32_t first) {
    d->fs = fs;
    d->first = first;
    d->ci = 0;
    d->cc = first;
    d->buf = NULL;
    d->grow = false;
}

void fat_dir_close(fat_dir_t *d) {
    if (d->buf) { buffer_release(d->buf); d->buf = NULL; }
}

/* Pointer to slot `idx`, valid until the next slot() call. NULL with *err == 0
 * means end of directory; with d->grow the chain is extended instead. */
static uint8_t *slot(fat_dir_t *d, uint32_t idx, int *err) {
    fat_fs_t *fs = d->fs;
    uint32_t off = idx * FAT_ENT_SIZE, lba;
    *err = d->grow ? -ENOSPC : 0;
    if (idx >= MAX_DIR_SLOTS) return NULL;

    if (d->first == 0) {
        if (off >= fs->root_sectors * fs->bps) return NULL;
        lba = fs->root_lba + off / fs->bps;
    } else {
        uint32_t k = off / fs->cluster_bytes;
        if (k < d->ci) { d->ci = 0; d->cc = d->first; }
        while (d->ci < k) {
            uint32_t n = fat_next(fs, d->cc);
            if (!n) {
                if (!d->grow) { *err = 0; return NULL; }
                int c = fat_alloc(fs, d->cc, true);
                if (c < 0) { *err = c; return NULL; }
                n = (uint32_t)c;
            }
            d->cc = n;
            d->ci++;
        }
        lba = fat_cluster_lba(fs, d->cc);
        if (!lba) { *err = -EIO; return NULL; }
        lba += (off % fs->cluster_bytes) / fs->bps;
    }
    if (!d->buf || d->buf->block_number != lba) {
        fat_dir_close(d);
        d->buf = fat_buf(fs, lba);
        if (!d->buf) { *err = -EIO; return NULL; }
    }
    *err = 0;
    return d->buf->data + off % fs->bps;
}

/* ---- Names ---------------------------------------------------------------- */

static int utf8_to_ucs2(const char *s, uint16_t *out, int max) {
    int n = 0;
    while (*s) {
        uint32_t c = (uint8_t)*s++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80 && !extra) return -EINVAL;
        if (extra) {
            c &= 0x3F >> extra;
            while (extra--) {
                if ((*s & 0xC0) != 0x80) return -EINVAL;
                c = (c << 6) | ((uint8_t)*s++ & 0x3F);
            }
        }
        if (c > 0xFFFF || c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|", (int)c))) return -EINVAL;
        if (n == max) return -ENAMETOOLONG;
        out[n++] = (uint16_t)c;
    }
    return n;
}

static int ucs2_to_utf8(const uint16_t *u, int n, char *out) {
    int len = 0;
    for (int i = 0; i < n && u[i] != 0 && u[i] != 0xFFFF; i++) {
        uint16_t c = u[i];
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (len + need > VFS_NAME_MAX) return -1;
        if (need == 1) out[len++] = (char)c;
        else if (need == 2) { out[len++] = (char)(0xC0 | c >> 6); out[len++] = (char)(0x80 | (c & 0x3F)); }
        else {
            out[len++] = (char)(0xE0 | c >> 12);
            out[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[len] = '\0';
    return len;
}

static void short_name(const uint8_t *e, uint8_t nt, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        out[n++] = (nt & 0x08) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            out[n++] = (nt & 0x10) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out[n] = '\0';
}

static char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool name_eq(const char *a, const char *b) {
    while (*a && fold(*a) == fold(*b)) { a++; b++; }
    return *a == *b;
}

static bool sfn_char_ok(uint16_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c < 0x80 && strchr("!#$%&'()-@^_`{}~", (int)c));
}

/* Can `u` be stored as a plain 8.3 entry (possibly using the NT case bits)? */
static bool fit_sfn(const uint16_t *u, int n, uint8_t *sfn, uint8_t *nt) {
    int ld = -1;
    for (int i = 0; i < n; i++) if (u[i] == '.') ld = i;
    int blen = ld < 0 ? n : ld, elen = ld < 0 ? 0 : n - ld - 1;
    if (blen < 1 || blen > 8 || elen > 3 || (ld >= 0 && elen == 0)) return false;
    bool lower[2] = { false, false }, upper[2] = { false, false };
    memset(sfn, ' ', 11);
    for (int i = 0; i < n; i++) {
        if (i == ld) continue;
        int part = ld >= 0 && i > ld;
        uint16_t c = u[i];
        if (!sfn_char_ok(c)) return false;
        if (c >= 'a' && c <= 'z') { lower[part] = true; c -= 32; }
        else if (c >= 'A' && c <= 'Z') upper[part] = true;
        sfn[part ? 8 + (i - ld - 1) : i] = (uint8_t)c;
    }
    if ((lower[0] && upper[0]) || (lower[1] && upper[1])) return false;
    *nt = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return true;
}

/* Numeric tails below this are tracked in a bitmap; above it the highest tail seen plus one is used. */
#define TAIL_BITS 4096

static int tail_len(uint32_t k) {
    int n = 0;
    for (; k; k /= 10) n++;
    return n;
}

/* "BASE~N.EXT" for the tail k: the base is cut so that the name stays within 8 characters. */
static void alias_name(uint8_t *sfn, const uint8_t *base, int blen, const uint8_t *ext, uint32_t k) {
    int tl = tail_len(k);
    int keep = blen < 7 - tl ? blen : 7 - tl;

    memset(sfn, ' ', 11);
    memcpy(sfn, base, (size_t)keep);
    sfn[keep] = '~';
    for (int i = tl - 1, v = (int)k; i >= 0; i--, v /= 10) sfn[keep + 1 + i] = (uint8_t)('0' + v % 10);
    memcpy(sfn + 8, ext, 3);
}

/* Build a unique "BASE~N.EXT" alias for a name that needs a long-name entry. One scan of the
 * directory records which tails exist for this base and extension. */
static int make_alias(fat_fs_t *fs, uint32_t dir, const uint16_t *u, int n, uint8_t *sfn) {
    int ld = -1;
    for (int i = 0; i < n; i++) if (u[i] == '.') ld = i;
    uint8_t base[8], ext[3] = { ' ', ' ', ' ' };
    int blen = 0, elen = 0;
    for (int i = 0; i < n; i++) {
        uint16_t c = u[i];
        if (c == ' ' || c == '.') continue;
        c = c < 0x80 && sfn_char_ok(c) ? (uint16_t)(c >= 'a' && c <= 'z' ? c - 32 : c) : '_';
        if (ld >= 0 && i > ld) { if (elen < 3) ext[elen++] = (uint8_t)c; }
        else if (blen < 8) base[blen++] = (uint8_t)c;
    }
    if (blen == 0) base[blen++] = '_';

    uint8_t used[TAIL_BITS / 8] = { 0 };
    uint8_t cand[11];
    uint32_t top = 0, pos = 0;
    fat_dir_t d;
    fat_ent_t e;
    fat_dir_open(&d, fs, dir);
    int rc;
    while ((rc = fat_dir_next(&d, &pos, &e)) > 0) {
        const uint8_t *s = e.sfn;
        int p = 1;
        while (p < 8 && s[p] != '~') p++;
        uint32_t k = 0;
        int q = p + 1;
        while (p < 8 && q < 8 && s[q] >= '0' && s[q] <= '9') k = k * 10 + (uint32_t)(s[q++] - '0');
        if (p == 8 || q == p + 1 || k == 0 || (q < 8 && s[q] != ' ')) continue;
        alias_name(cand, base, blen, ext, k);       /* only the exact alias of this name and tail counts */
        if (memcmp(cand, s, 11) != 0) continue;
        if (k < TAIL_BITS) used[k / 8] |= (uint8_t)(1u << (k % 8));
        if (k > top) top = k;
    }
    fat_dir_close(&d);
    if (rc < 0) return rc;

    uint32_t k = 1;
    while (k < TAIL_BITS && (used[k / 8] >> (k % 8) & 1)) k++;
    if (k == TAIL_BITS) k = top + 1;
    if (k > 999999) return -ENOSPC;
    alias_name(sfn, base, blen, ext, k);
    return 0;
}

/* ---- Iteration and lookup ------------------------------------------------- */

/* Everything an 8.3 slot says about the entry (not its name or position). */
static void ent_decode(const fat_fs_t *fs, const uint8_t *e, fat_ent_t *out) {
    const fat_dirent_t *de = (const fat_dirent_t *)e;
    memcpy(out->sfn, e, 11);
    out->attr = de->attr;
    out->cluster = de->cluster_lo | (fs->type == 32 ? (uint32_t)de->cluster_hi << 16 : 0);
    out->size = de->size;
    out->mtime = fat_dos_to_unix(de->mdate, de->mtime);
    out->atime = fat_dos_to_unix(de->adate, 0);
    out->ctime = fat_dos_to_unix(de->cdate, de->ctime);
}

int fat_dir_next(fat_dir_t *d, uint32_t *pos, fat_ent_t *out) {
    uint16_t u[MAX_LFN_CHARS + 1];
    uint32_t first = 0;
    int want = 0;
    uint8_t csum = 0;
    bool have = false;

    for (uint32_t i = *pos;; i++) {
        int err;
        uint8_t *e = slot(d, i, &err);
        if (!e) return err;
        if (e[0] == 0x00) return 0;
        if (e[0] == 0xE5) { have = false; continue; }

        if (e[11] == FAT_ATTR_LFN) {
            int s = e[0] & 0x3F;
            if (e[0] & 0x40) {
                if (s < 1 || s > MAX_LFN_SLOTS) { have = false; continue; }
                have = true;
                want = s;
                csum = e[13];
                first = i;
                u[s * 13] = 0;
            } else if (!have || s != want || e[13] != csum) {
                have = false;
                continue;
            }
            for (int k = 0; k < 13; k++) memcpy(&u[(s - 1) * 13 + k], e + lfn_off[k], 2);
            want = s - 1;
            continue;
        }
        if (e[11] & FAT_ATTR_VOLID) { have = false; continue; }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { have = false; continue; }

        const fat_dirent_t *de = (const fat_dirent_t *)e;
        bool long_name = have && want == 0 && lfn_csum(e) == csum;
        if (long_name) {
            if (ucs2_to_utf8(u, MAX_LFN_CHARS, out->name) < 0) { have = false; continue; }
        } else {
            short_name(e, de->nt, out->name);
        }
        ent_decode(d->fs, e, out);
        out->idx = i;
        out->first_idx = long_name ? first : i;
        out->lba = d->buf->block_number;
        out->off = (i * FAT_ENT_SIZE) % d->fs->bps;
        *pos = i + 1;
        return 1;
    }
}

/* ---- Name cache ------------------------------------------------------------ */

/* Remembers where a name's slot is; the slot stays authoritative, so size, cluster and times are read from
 * it. Entries are dropped when their slot is freed (fat_dir_remove) and replaced when a name is created. */
static fat_dcache_t *dc_slot(const fat_fs_t *fs, uint32_t dir, const char *name) {
    uint32_t h = dir * 2654435761u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)fold(*p)) * 16777619u;
    return strlen(name) < sizeof(fs->dcache->name) ? &fs->dcache[(h ^ h >> 15) % FAT_DCACHE_SIZE] : NULL;
}

static void dc_put(fat_fs_t *fs, uint32_t dir, const char *name, const fat_ent_t *e) {
    fat_dcache_t *c = dc_slot(fs, dir, name);
    if (!c) return;
    c->dir = dir;
    c->lba = e->lba;
    c->off = e->off;
    c->idx = e->idx;
    c->first_idx = e->first_idx;
    memcpy(c->sfn, e->sfn, 11);
    strcpy(c->name, name);
}

static void dc_drop(fat_fs_t *fs, uint32_t dir, const char *name) {
    fat_dcache_t *c = dc_slot(fs, dir, name);
    if (c && c->dir == dir && name_eq(c->name, name)) c->name[0] = '\0';
}

/* The cached slot of (dir, name) if it still holds that entry. */
static bool dc_get(fat_fs_t *fs, uint32_t dir, const char *name, fat_ent_t *out) {
    fat_dcache_t *c = dc_slot(fs, dir, name);
    if (!c || !c->name[0] || c->dir != dir || !name_eq(c->name, name)) return false;
    buffer_t *b = fat_buf(fs, c->lba);
    if (!b) return false;
    const uint8_t *e = b->data + c->off;
    bool ok = e[0] != 0x00 && e[0] != 0xE5 && e[11] != FAT_ATTR_LFN && memcmp(e, c->sfn, 11) == 0;
    if (ok) {
        ent_decode(fs, e, out);
        strcpy(out->name, name);
        out->idx = c->idx;
        out->first_idx = c->first_idx;
        out->lba = c->lba;
        out->off = c->off;
    } else {
        c->name[0] = '\0';
    }
    buffer_release(b);
    return ok;
}

int fat_dir_find(fat_fs_t *fs, uint32_t dir, const char *name, fat_ent_t *out) {
    if (dc_get(fs, dir, name, out)) return 0;
    fat_dir_t d;
    uint32_t pos = 0;
    int rc;
    fat_dir_open(&d, fs, dir);
    while ((rc = fat_dir_next(&d, &pos, out)) > 0 && !name_eq(out->name, name)) {}
    fat_dir_close(&d);
    if (rc > 0) dc_put(fs, dir, out->name, out);
    return rc > 0 ? 0 : rc < 0 ? rc : -ENOENT;
}

int fat_dir_is_empty(fat_fs_t *fs, uint32_t dir) {
    fat_dir_t d;
    fat_ent_t e;
    uint32_t pos = 0;
    fat_dir_open(&d, fs, dir);
    int rc = fat_dir_next(&d, &pos, &e);
    fat_dir_close(&d);
    return rc < 0 ? rc : rc == 0;
}

/* ---- Modification --------------------------------------------------------- */

int fat_dir_add(fat_fs_t *fs, uint32_t dir, const char *name, uint8_t attr,
                uint32_t cluster, uint32_t size, uint32_t mtime, fat_ent_t *out) {
    uint16_t u[VFS_NAME_MAX];
    uint8_t sfn[11], nt = 0;
    int n = utf8_to_ucs2(name, u, VFS_NAME_MAX);
    if (n < 0) return n;
    if (n == 0 || (n == 1 && u[0] == '.') || (n == 2 && u[0] == '.' && u[1] == '.')) return -EINVAL;

    int nlfn = 0;
    if (!fit_sfn(u, n, sfn, &nt)) {
        nlfn = (n + 12) / 13;
        int rc = make_alias(fs, dir, u, n, sfn);
        if (rc < 0) return rc;
    }

    /* Find nlfn+1 consecutive free slots; everything after an end marker is free. */
    fat_dir_t d;
    fat_dir_open(&d, fs, dir);
    d.grow = true;
    uint32_t need = (uint32_t)nlfn + 1, run = 0, start = 0;
    bool end = false;
    int rc = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t *e = slot(&d, i, &rc);
        if (!e) goto out;
        if (e[0] == 0x00) end = true;
        if (end || e[0] == 0xE5) {
            if (run++ == 0) start = i;
            if (run == need) break;
        } else {
            run = 0;
        }
    }

    uint8_t cs = lfn_csum(sfn);
    for (int k = 0; k < nlfn; k++) {
        uint8_t *e = slot(&d, start + (uint32_t)k, &rc);
        if (!e) goto out;
        int seq = nlfn - k, base = (seq - 1) * 13;
        memset(e, 0, FAT_ENT_SIZE);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = FAT_ATTR_LFN;
        e[13] = cs;
        for (int j = 0; j < 13; j++) {
            uint16_t c = base + j < n ? u[base + j] : base + j == n ? 0 : 0xFFFF;
            memcpy(e + lfn_off[j], &c, 2);
        }
        buffer_mark_dirty(d.buf);
    }

    uint32_t idx = start + (uint32_t)nlfn;
    uint8_t *e = slot(&d, idx, &rc);
    if (!e) goto out;
    fat_dirent_t *de = (fat_dirent_t *)e;
    uint16_t date, time;
    fat_unix_to_dos(mtime, &date, &time);
    memset(de, 0, sizeof(*de));
    memcpy(de->name, sfn, 11);
    de->attr = attr;
    de->nt = nt;
    de->ctime = de->mtime = time;
    de->cdate = de->mdate = de->adate = date;
    de->cluster_lo = (uint16_t)cluster;
    de->cluster_hi = fs->type == 32 ? (uint16_t)(cluster >> 16) : 0;
    de->size = size;
    buffer_mark_dirty(d.buf);

    fat_ent_t made;
    if (!out) out = &made;
    strncpy(out->name, name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    memcpy(out->sfn, sfn, 11);
    out->attr = attr;
    out->cluster = cluster;
    out->size = size;
    out->mtime = out->atime = out->ctime = mtime;
    out->idx = idx;
    out->first_idx = start;
    out->lba = d.buf->block_number;
    out->off = (idx * FAT_ENT_SIZE) % fs->bps;
    dc_put(fs, dir, name, out);
out:
    fat_dir_close(&d);
    return rc;
}

/* Mark the 8.3 entry and its long-name slots deleted. */
int fat_dir_remove(fat_fs_t *fs, uint32_t dir, const fat_ent_t *ent) {
    fat_dir_t d;
    int rc = 0;
    fat_dir_open(&d, fs, dir);
    for (uint32_t i = ent->first_idx; i <= ent->idx; i++) {
        uint8_t *e = slot(&d, i, &rc);
        if (!e) { if (rc == 0) rc = -EIO; break; }
        e[0] = 0xE5;
        buffer_mark_dirty(d.buf);
    }
    fat_dir_close(&d);
    dc_drop(fs, dir, ent->name);
    return rc;
}

int fat_ent_update(fat_fs_t *fs, uint32_t lba, uint32_t off, uint32_t cluster,
                   uint32_t size, uint32_t mtime) {
    buffer_t *b = fat_buf(fs, lba);
    if (!b) return -EIO;
    fat_dirent_t *de = (fat_dirent_t *)(b->data + off);
    uint16_t date, time;
    fat_unix_to_dos(mtime, &date, &time);
    de->cluster_lo = (uint16_t)cluster;
    de->cluster_hi = fs->type == 32 ? (uint16_t)(cluster >> 16) : 0;
    de->size = size;
    de->mtime = time;
    de->mdate = de->adate = date;
    buffer_mark_dirty(b);
    buffer_release(b);
    return 0;
}
