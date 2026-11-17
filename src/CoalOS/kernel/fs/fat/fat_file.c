/**
 * @file fat_file.c
 * @brief FAT as a VFS driver: open files (shared nodes), data I/O and the
 *        namespace operations.
 */
#define LOG_MODULE "fat"
#include <kernel/core/log.h>
#include <kernel/fs/fat/fat.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include "fat_priv.h"


/* ---- Open nodes ----------------------------------------------------------- */

static fat_node_t *node_find(fat_fs_t *fs, uint32_t lba, uint32_t off) {
    for (fat_node_t *n = fs->nodes; n; n = n->next)
        if (n->ent_lba == lba && n->ent_off == off) return n;
    return NULL;
}

/* Take a reference on the node for `e`, creating it if nobody has it open. */
static fat_node_t *node_get(fat_fs_t *fs, const fat_ent_t *e) {
    fat_node_t *n = node_find(fs, e->lba, e->off);
    if (!n) {
        n = kmalloc(sizeof(*n));
        if (!n) return NULL;
        memset(n, 0, sizeof(*n));
        n->fs = fs;
        n->first = e->cluster;
        n->size = e->size;
        n->ent_lba = e->lba;
        n->ent_off = e->off;
        n->attr = e->attr;
        n->mtime = e->mtime;
        n->atime = e->atime;
        n->ctime = e->ctime;
        n->next = fs->nodes;
        fs->nodes = n;
    }
    n->ref++;
    return n;
}

static void fill_root(fat_fs_t *fs, fat_ent_t *e) {
    memset(e, 0, sizeof(*e));
    e->attr = FAT_ATTR_DIR;
    e->cluster = fat_root(fs);
}

static int node_flush_entry(fat_node_t *n) {
    if (n->ent_lba == FAT_NODE_ROOT_LBA || n->ent_lba == FAT_NODE_UNLINKED) return 0;
    return fat_ent_update(n->fs, n->ent_lba, n->ent_off, n->first, n->size, n->mtime);
}

static bool is_dir(const fat_node_t *n) { return n->attr & FAT_ATTR_DIR; }

/* ---- Path resolution ------------------------------------------------------ */

/* Walk all but the last component; `dir` is the parent directory, `leaf` the
 * last component (empty for the root). */
static int resolve_parent(fat_fs_t *fs, const char *p, uint32_t *dir, char *leaf) {
    *dir = fat_root(fs);
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { leaf[0] = '\0'; return 0; }
        size_t len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len > VFS_NAME_MAX) return -ENAMETOOLONG;
        memcpy(leaf, p, len);
        leaf[len] = '\0';
        p += len;
        while (*p == '/') p++;
        if (!*p) return 0;
        fat_ent_t e;
        int rc = fat_dir_find(fs, *dir, leaf, &e);
        if (rc < 0) return rc;
        if (!(e.attr & FAT_ATTR_DIR)) return -ENOTDIR;
        *dir = e.cluster;
    }
}

/* ---- Data I/O ------------------------------------------------------------- */

static uint32_t chain_at(const fat_node_t *n, uint32_t idx) {
    if (n->hint_cl && idx >= n->hint_idx) return fat_nth(n->fs, n->hint_cl, idx - n->hint_idx);
    return fat_nth(n->fs, n->first, idx);
}

/* Cluster number for cluster index `idx` of the file, appending one if asked. */
static int node_cluster(fat_node_t *n, uint32_t idx, bool alloc) {
    uint32_t cl = chain_at(n, idx);
    if (!cl && alloc) {
        uint32_t prev = idx ? chain_at(n, idx - 1) : 0;
        if (idx && !prev) return -EIO;
        int c = fat_alloc(n->fs, prev);
        if (c < 0) return c;
        if (!prev) n->first = (uint32_t)c;
        cl = (uint32_t)c;
    }
    if (!cl) return -EIO;
    n->hint_idx = idx;
    n->hint_cl = cl;
    return (int)cl;
}

/* Move bytes between buf and the file at pos (buf == NULL writes zeros). */
static ssize_t node_rw(fat_node_t *n, uint32_t pos, uint8_t *buf, uint32_t len, bool write) {
    fat_fs_t *fs = n->fs;
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        uint32_t off = pos + done;
        int cl = node_cluster(n, off / fs->cluster_bytes, write);
        if (cl < 0) { err = cl; break; }
        uint32_t in = off % fs->cluster_bytes;
        uint32_t chunk = MIN(fs->bps - in % fs->bps, len - done);
        buffer_t *b = fat_buf(fs, fat_cluster_lba(fs, (uint32_t)cl) + in / fs->bps);
        if (!b) { err = -EIO; break; }
        uint8_t *p = b->data + in % fs->bps;
        if (!write) memcpy(buf + done, p, chunk);
        else {
            if (buf) memcpy(p, buf + done, chunk); else memset(p, 0, chunk);
            buffer_mark_dirty(b);
        }
        buffer_release(b);
        done += chunk;
    }
    return done ? (ssize_t)done : err;
}

static ssize_t fat_read(vnode_t *v, off_t pos, void *buf, size_t len) {
    fat_node_t *n = v->data;
    fat_lock(n->fs);
    ssize_t rc = 0;
    if ((uint32_t)pos < n->size) rc = node_rw(n, (uint32_t)pos, buf, (uint32_t)MIN(len, n->size - (uint32_t)pos), false);
    fat_unlock(n->fs);
    return rc;
}

static ssize_t write_locked(fat_node_t *n, uint32_t pos, const void *buf, uint32_t len) {
    ssize_t rc = 0;
    if (pos > n->size) {                         /* zero-fill the gap */
        rc = node_rw(n, n->size, NULL, pos - n->size, true);
        if (rc < 0) return rc;
        n->size = pos;
    }
    rc = node_rw(n, pos, (uint8_t *)buf, len, true);
    if (rc > 0 && pos + (uint32_t)rc > n->size) n->size = pos + (uint32_t)rc;
    n->mtime = vfs_time();
    n->dirty = true;
    n->fs->unsynced = true;
    int e = node_flush_entry(n);
    return rc < 0 ? rc : e < 0 ? e : rc;
}

static ssize_t fat_write(vnode_t *v, off_t pos, const void *buf, size_t len) {
    fat_node_t *n = v->data;
    if (len == 0) return 0;
    if ((uint64_t)pos + len > 0xFFFFFFFFull) return -EFBIG;
    fat_lock(n->fs);
    ssize_t rc = write_locked(n, (uint32_t)pos, buf, (uint32_t)len);
    fat_unlock(n->fs);
    return rc;
}

static int fat_truncate(vnode_t *v, off_t len) {
    fat_node_t *n = v->data;
    fat_fs_t *fs = n->fs;
    if (is_dir(n)) return -EISDIR;
    fat_lock(fs);
    int rc = 0;
    if ((uint32_t)len > n->size) {
        uint32_t delta = (uint32_t)len - n->size;
        ssize_t w = write_locked(n, n->size, NULL, delta);
        rc = w < 0 ? (int)w : (uint32_t)w == delta ? 0 : -ENOSPC;
    } else if ((uint32_t)len < n->size) {
        uint32_t keep = ((uint32_t)len + fs->cluster_bytes - 1) / fs->cluster_bytes;
        uint32_t tail = keep * fs->cluster_bytes - (uint32_t)len;
        if (tail) node_rw(n, (uint32_t)len, NULL, MIN(tail, n->size - (uint32_t)len), true);
        uint32_t rest;
        if (keep == 0) { rest = n->first; n->first = 0; }
        else {
            uint32_t last = fat_nth(fs, n->first, keep - 1);
            rest = fat_next(fs, last);
            if (last) fat_set_eoc(fs, last);
        }
        n->size = (uint32_t)len;
        n->hint_cl = 0;
        n->mtime = vfs_time();
        rc = node_flush_entry(n);               /* entry first, then release the clusters */
        fat_free_chain(fs, rest);
    }
    n->dirty = true;
    if (rc == 0) rc = fat_commit(fs);
    fat_unlock(fs);
    return rc;
}

/* ---- VFS operations ------------------------------------------------------- */

static int open_locked(fat_fs_t *fs, const char *path, int flags, vnode_t *v) {
    char leaf[VFS_NAME_MAX + 1];
    uint32_t dir;
    fat_ent_t e;
    bool created = false;
    int rc = resolve_parent(fs, path, &dir, leaf);
    if (rc < 0) return rc;

    if (!leaf[0]) fill_root(fs, &e);
    else {
        rc = fat_dir_find(fs, dir, leaf, &e);
        if (rc == -ENOENT && (flags & O_CREAT)) {
            rc = fat_dir_add(fs, dir, leaf, FAT_ATTR_ARCH, 0, 0, vfs_time(), &e);
            created = rc == 0;
        } else if (rc == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
            rc = -EEXIST;
        } else if (rc == 0 && (flags & O_ACCMODE) != O_RDONLY && (e.attr & FAT_ATTR_RO)) {
            rc = -EACCES;
        }
        if (rc < 0) return rc;
    }
    fat_node_t *n = node_get(fs, &e);
    if (!n) return -ENOMEM;
    v->data = n;
    v->type = (e.attr & FAT_ATTR_DIR) ? S_IFDIR : S_IFREG;
    return created ? fat_commit(fs) : 0;
}

static int fat_open(void *ctx, const char *path, int flags, mode_t mode, vnode_t *v) {
    fat_fs_t *fs = ctx;
    fat_lock(fs);
    int rc = open_locked(fs, path, flags, v);
    fat_unlock(fs);
    return rc;
}

static int fat_close(vnode_t *v) {
    fat_node_t *n = v->data;
    fat_fs_t *fs = n->fs;
    int rc = 0;
    fat_lock(fs);
    if (--n->ref == 0) {
        if (n->ent_lba == FAT_NODE_UNLINKED) fat_free_chain(fs, n->first);    /* last user of a removed file */
        if (n->dirty || n->ent_lba == FAT_NODE_UNLINKED) rc = fat_commit(fs);
        for (fat_node_t **pp = &fs->nodes; *pp; pp = &(*pp)->next) {
            if (*pp == n) { *pp = n->next; break; }
        }
        kfree(n);
    }
    fat_unlock(fs);
    return rc;
}

static int fat_utime(vnode_t *v, uint32_t atime, uint32_t mtime) {
    fat_node_t *n = v->data;
    fat_fs_t *fs = n->fs;

    fat_lock(fs);
    n->atime = atime;                       /* kept in memory only: FAT stores the access date, not the time */
    n->mtime = mtime;
    int rc = node_flush_entry(n);
    if (rc == 0) rc = fat_commit(fs);
    fat_unlock(fs);
    return rc;
}

static int fat_fsync(vnode_t *v) { return fat_sync(((fat_node_t *)v->data)->fs); }

static int fat_getattr(vnode_t *v, struct kstat64 *st) {
    fat_node_t *n = v->data;
    fat_fs_t *fs = n->fs;
    bool dir = is_dir(n);
    st->st_dev = 1;
    st->st_ino = n->ent_lba ? (uint64_t)n->ent_lba * (fs->bps / FAT_ENT_SIZE) + n->ent_off / FAT_ENT_SIZE : 1;
    st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | ((n->attr & FAT_ATTR_RO) ? 0555 : 0755);
    st->st_nlink = n->ent_lba == FAT_NODE_UNLINKED ? 0 : dir ? 2 : 1;
    st->st_size = dir ? 0 : n->size;
    st->st_blksize = 4096;
    st->st_blocks = dir ? 0 : (uint64_t)((n->size + fs->cluster_bytes - 1) / fs->cluster_bytes) * fs->spc * (fs->bps / 512);
    st->st_atime = n->atime;
    st->st_mtime = n->mtime;
    st->st_ctime = n->ctime;
    return 0;
}

static int fat_readdir(vnode_t *v, off_t *pos, struct vfs_dirent *out) {
    fat_node_t *n = v->data;
    fat_fs_t *fs = n->fs;
    if (*pos < 2) {
        strcpy(out->name, *pos == 0 ? "." : "..");
        out->ino = n->ent_lba ? n->ent_lba * (fs->bps / FAT_ENT_SIZE) + n->ent_off / FAT_ENT_SIZE : 1;
        out->type = DT_DIR;
        (*pos)++;
        return 1;
    }
    fat_dir_t d;
    fat_ent_t e;
    uint32_t i = (uint32_t)*pos - 2;
    fat_lock(fs);
    fat_dir_open(&d, fs, n->first);
    int rc = fat_dir_next(&d, &i, &e);
    fat_dir_close(&d);
    fat_unlock(fs);
    if (rc <= 0) return rc;
    strcpy(out->name, e.name);
    out->ino = e.lba * (fs->bps / FAT_ENT_SIZE) + e.off / FAT_ENT_SIZE;
    out->type = (e.attr & FAT_ATTR_DIR) ? DT_DIR : DT_REG;
    *pos = (off_t)i + 2;
    return 1;
}

/* Locate `path` and its parent; the root has no entry (rc == -EBUSY). */
static int lookup(fat_fs_t *fs, const char *path, uint32_t *dir, char *leaf, fat_ent_t *e) {
    int rc = resolve_parent(fs, path, dir, leaf);
    if (rc < 0) return rc;
    if (!leaf[0]) return -EBUSY;
    return fat_dir_find(fs, *dir, leaf, e);
}

/* The directory entry e was removed: free its clusters now, or when the last opener closes the file. */
static void entry_released(fat_fs_t *fs, const fat_ent_t *e) {
    fat_node_t *n = node_find(fs, e->lba, e->off);

    if (n) n->ent_lba = FAT_NODE_UNLINKED;
    else fat_free_chain(fs, e->cluster);
}

static int fat_unlink(void *ctx, const char *path) {
    fat_fs_t *fs = ctx;
    char leaf[VFS_NAME_MAX + 1];
    uint32_t dir;
    fat_ent_t e;
    fat_lock(fs);
    int rc = lookup(fs, path, &dir, leaf, &e);
    if (rc == -EBUSY) rc = -EISDIR;
    else if (rc == 0 && (e.attr & FAT_ATTR_DIR)) rc = -EISDIR;
    if (rc == 0) rc = fat_dir_remove(fs, dir, &e);      /* entry first, then the chain */
    if (rc == 0) { entry_released(fs, &e); rc = fat_commit(fs); }
    fat_unlock(fs);
    return rc;
}

static int fat_rmdir(void *ctx, const char *path) {
    fat_fs_t *fs = ctx;
    char leaf[VFS_NAME_MAX + 1];
    uint32_t dir;
    fat_ent_t e;
    fat_lock(fs);
    int rc = lookup(fs, path, &dir, leaf, &e);
    if (rc == 0) {
        if (!(e.attr & FAT_ATTR_DIR)) rc = -ENOTDIR;
        else {
            rc = fat_dir_is_empty(fs, e.cluster);
            if (rc == 0) rc = -ENOTEMPTY;
            else if (rc > 0) rc = 0;
        }
    }
    if (rc == 0) rc = fat_dir_remove(fs, dir, &e);
    if (rc == 0) { entry_released(fs, &e); rc = fat_commit(fs); }    /* an open directory stays, empty, until closed */
    fat_unlock(fs);
    return rc;
}

/* "." and ".." entries of a new directory; ".." is 0 when the parent is the root. */
static int init_dir_cluster(fat_fs_t *fs, uint32_t cluster, uint32_t parent) {
    buffer_t *b = fat_buf(fs, fat_cluster_lba(fs, cluster));
    if (!b) return -EIO;
    uint16_t date, time;
    fat_unix_to_dos(vfs_time(), &date, &time);
    for (int i = 0; i < 2; i++) {
        fat_dirent_t *de = (fat_dirent_t *)b->data + i;
        uint32_t c = i == 0 ? cluster : (parent == fat_root(fs) ? 0 : parent);
        memset(de, 0, sizeof(*de));
        memset(de->name, ' ', 11);
        de->name[0] = '.';
        if (i) de->name[1] = '.';
        de->attr = FAT_ATTR_DIR;
        de->ctime = de->mtime = time;
        de->cdate = de->mdate = de->adate = date;
        de->cluster_lo = (uint16_t)c;
        de->cluster_hi = fs->type == 32 ? (uint16_t)(c >> 16) : 0;
    }
    buffer_mark_dirty(b);
    buffer_release(b);
    return 0;
}

static int fat_mkdir(void *ctx, const char *path, mode_t mode) {
    fat_fs_t *fs = ctx;
    char leaf[VFS_NAME_MAX + 1];
    uint32_t dir;
    fat_ent_t e;
    fat_lock(fs);
    int rc = resolve_parent(fs, path, &dir, leaf);
    if (rc == 0 && !leaf[0]) rc = -EEXIST;
    if (rc == 0) {
        int f = fat_dir_find(fs, dir, leaf, &e);
        rc = f == 0 ? -EEXIST : f == -ENOENT ? 0 : f;
    }
    if (rc == 0) {
        int c = fat_alloc(fs, 0);
        rc = c < 0 ? c : init_dir_cluster(fs, (uint32_t)c, dir);
        if (rc == 0) rc = fat_dir_add(fs, dir, leaf, FAT_ATTR_DIR, (uint32_t)c, 0, vfs_time(), &e);
        if (rc < 0 && c > 0) fat_free_chain(fs, (uint32_t)c);
    }
    if (rc == 0) rc = fat_commit(fs);
    fat_unlock(fs);
    return rc;
}

static bool path_inside(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static int rename_locked(fat_fs_t *fs, const char *from, const char *to) {
    char sleaf[VFS_NAME_MAX + 1], dleaf[VFS_NAME_MAX + 1];
    uint32_t sdir, ddir;
    fat_ent_t s, d;
    int rc = lookup(fs, from, &sdir, sleaf, &s);
    if (rc < 0) return rc;
    bool sdirp = s.attr & FAT_ATTR_DIR;
    if (sdirp && path_inside(to, from)) return -EINVAL;

    rc = resolve_parent(fs, to, &ddir, dleaf);
    if (rc < 0) return rc;
    if (!dleaf[0]) return -EBUSY;

    rc = fat_dir_find(fs, ddir, dleaf, &d);
    if (rc == 0 && d.lba == s.lba && d.off == s.off) {
        /* same entry: only the spelling changes */
    } else if (rc == 0) {
        bool ddirp = d.attr & FAT_ATTR_DIR;
        if (ddirp != sdirp) return ddirp ? -EISDIR : -ENOTDIR;
        if (ddirp) {
            int empty = fat_dir_is_empty(fs, d.cluster);
            if (empty < 0) return empty;
            if (!empty) return -ENOTEMPTY;
        }
        rc = fat_dir_remove(fs, ddir, &d);
        if (rc < 0) return rc;
        entry_released(fs, &d);
    } else if (rc != -ENOENT) {
        return rc;
    }

    fat_ent_t n;
    rc = fat_dir_add(fs, ddir, dleaf, s.attr, s.cluster, s.size, s.mtime, &n);
    if (rc < 0) return rc;
    fat_node_t *open = node_find(fs, s.lba, s.off);     /* an open file follows its entry */
    if (open) { open->ent_lba = n.lba; open->ent_off = n.off; }
    if (sdirp && sdir != ddir) {                 /* repoint ".." */
        buffer_t *b = fat_buf(fs, fat_cluster_lba(fs, s.cluster));
        if (b) {
            fat_dirent_t *dd = (fat_dirent_t *)b->data + 1;
            uint32_t p = ddir == fat_root(fs) ? 0 : ddir;
            dd->cluster_lo = (uint16_t)p;
            dd->cluster_hi = fs->type == 32 ? (uint16_t)(p >> 16) : 0;
            buffer_mark_dirty(b);
            buffer_release(b);
        }
    }
    return fat_dir_remove(fs, sdir, &s);        /* new entry exists before the old one goes */
}

static int fat_rename(void *ctx, const char *from, const char *to) {
    fat_fs_t *fs = ctx;
    fat_lock(fs);
    int rc = rename_locked(fs, from, to);
    if (rc == 0) rc = fat_commit(fs);
    fat_unlock(fs);
    return rc;
}

static int fat_statfs(void *ctx, struct kstatfs64 *st) {
    fat_fs_t *fs = ctx;

    fat_lock(fs);
    st->f_bsize = st->f_frsize = fs->cluster_bytes;
    st->f_blocks = fs->nclusters;
    st->f_bfree = st->f_bavail = fat_free_clusters(fs);
    fat_unlock(fs);
    return 0;
}

vfs_driver_t fat_driver = {
    .fs_name  = "fat",
    .magic    = 0x4d44,                             /* MSDOS_SUPER_MAGIC */
    .statfs   = fat_statfs,
    .mount    = fat_mount,
    .unmount  = fat_unmount,
    .sync     = fat_sync,
    .open     = fat_open,
    .close    = fat_close,
    .read     = fat_read,
    .write    = fat_write,
    .readdir  = fat_readdir,
    .getattr  = fat_getattr,
    .truncate = fat_truncate,
    .fsync    = fat_fsync,
    .utime    = fat_utime,
    .unlink   = fat_unlink,
    .mkdir    = fat_mkdir,
    .rmdir    = fat_rmdir,
    .rename   = fat_rename,
};

int fat_register_driver(void) { return vfs_register_driver(&fat_driver); }
