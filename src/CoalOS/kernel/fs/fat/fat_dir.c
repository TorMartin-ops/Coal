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
                int c = fat_alloc(fs, d->cc);
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

static bool sfn_taken(fat_fs_t *fs, uint32_t dir, const uint8_t *sfn) {
    fat_dir_t d;
    fat_ent_t e;
    uint32_t pos = 0;
    bool taken = false;
    fat_dir_open(&d, fs, dir);
    while (!taken && fat_dir_next(&d, &pos, &e) > 0) taken = memcmp(e.sfn, sfn, 11) == 0;
    fat_dir_close(&d);
    return taken;
}

/* Build a unique "BASE~N.EXT" alias for a name that needs a long-name entry. */
static int make_alias(fat_fs_t *fs, uint32_t dir, const uint16_t *u, int n, uint8_t *sfn) {
    int ld = -1;
    for (int i = 0; i < n; i++) if (u[i] == '.') ld = i;
    uint8_t base[8], ext[3];
    int blen = 0, elen = 0;
    for (int i = 0; i < n; i++) {
        uint16_t c = u[i];
        if (c == ' ' || c == '.') continue;
        c = c < 0x80 && sfn_char_ok(c) ? (uint16_t)(c >= 'a' && c <= 'z' ? c - 32 : c) : '_';
        if (ld >= 0 && i > ld) { if (elen < 3) ext[elen++] = (uint8_t)c; }
        else if (blen < 8) base[blen++] = (uint8_t)c;
    }
    if (blen == 0) base[blen++] = '_';

    for (uint32_t k = 1; k < 1000000; k++) {
        char tag[8];
        int tl = 0;
        for (uint32_t v = k; v; v /= 10) tag[tl++] = (char)('0' + v % 10);
        int keep = blen < 7 - tl ? blen : 7 - tl;
        memset(sfn, ' ', 11);
        memcpy(sfn, base, (size_t)keep);
        sfn[keep] = '~';
        for (int i = 0; i < tl; i++) sfn[keep + 1 + i] = (uint8_t)tag[tl - 1 - i];
        memcpy(sfn + 8, ext, (size_t)elen);
        if (!sfn_taken(fs, dir, sfn)) return 0;
    }
    return -ENOSPC;
}

/* ---- Iteration and lookup ------------------------------------------------- */

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
        memcpy(out->sfn, e, 11);
        out->attr = de->attr;
        out->cluster = de->cluster_lo | (d->fs->type == 32 ? (uint32_t)de->cluster_hi << 16 : 0);
        out->size = de->size;
        out->mtime = fat_dos_to_unix(de->mdate, de->mtime);
        out->atime = fat_dos_to_unix(de->adate, 0);
        out->ctime = fat_dos_to_unix(de->cdate, de->ctime);
        out->idx = i;
        out->first_idx = long_name ? first : i;
        out->lba = d->buf->block_number;
        out->off = (i * FAT_ENT_SIZE) % d->fs->bps;
        *pos = i + 1;
        return 1;
    }
}

int fat_dir_find(fat_fs_t *fs, uint32_t dir, const char *name, fat_ent_t *out) {
    fat_dir_t d;
    uint32_t pos = 0;
    int rc;
    fat_dir_open(&d, fs, dir);
    while ((rc = fat_dir_next(&d, &pos, out)) > 0 && !name_eq(out->name, name)) {}
    fat_dir_close(&d);
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

    if (out) {
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
    }
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
