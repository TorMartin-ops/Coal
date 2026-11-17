/**
 * @file vfs.c
 * @brief VFS core: driver registry, mount table, path handling, file API.
 *
 * Slow driver work (disk I/O) never runs under the mount-table lock: the lock
 * only guards table updates and mount reference counts.
 */
#define LOG_MODULE "vfs"
#include <kernel/core/log.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/timer/rtc.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/sync/spinlock.h>

#define MAX_DRIVERS   6
#define MAX_MOUNTS    8
#define MOUNT_PATH_MAX 48
#define IO_MAX        0x7ffff000u

struct mount {
    char          path[MOUNT_PATH_MAX];
    char          dev[8];           /* backing device, empty for virtual file systems */
    size_t        plen;
    vfs_driver_t *drv;
    void         *ctx;
    int           refs;             /* open files + operations in flight */
    bool          used;
    bool          dying;
};

static vfs_driver_t *drivers[MAX_DRIVERS];
static mount_t mounts[MAX_MOUNTS];
static spinlock_t mount_lock;

uint32_t vfs_time(void) { return wall_seconds(); }

void vfs_init(void) {
    spinlock_init(&mount_lock);
}

int vfs_register_driver(vfs_driver_t *drv) {
    if (!drv || !drv->fs_name || !drv->open || !drv->close) return -EINVAL;
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] && strcmp(drivers[i]->fs_name, drv->fs_name) == 0) return -EEXIST;
    }
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (!drivers[i]) { drivers[i] = drv; return 0; }
    }
    return -ENOSPC;
}

static vfs_driver_t *find_driver(const char *name) {
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (drivers[i] && strcmp(drivers[i]->fs_name, name) == 0) return drivers[i];
    }
    return NULL;
}

/* ---- Paths ---------------------------------------------------------------- */

int vfs_normalize(const char *in, char *out, size_t size) {
    if (!in || in[0] != '/' || size < 2) return -EINVAL;
    size_t len = 1;
    out[0] = '/';
    while (*in) {
        while (*in == '/') in++;
        const char *c = in;
        while (*in && *in != '/') in++;
        size_t clen = (size_t)(in - c);
        if (clen == 0 || (clen == 1 && c[0] == '.')) continue;
        if (clen == 2 && c[0] == '.' && c[1] == '.') {
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            continue;
        }
        if (clen > VFS_NAME_MAX || len + clen + 2 > size) return -ENAMETOOLONG;
        if (len > 1) out[len++] = '/';
        memcpy(out + len, c, clen);
        len += clen;
    }
    out[len] = '\0';
    return 0;
}

/* Normalized copy of path on the heap (paths are too large for the kernel stack); NULL with *rc set on failure. */
static char *norm_alloc(const char *path, int *rc) {
    char *norm = kmalloc(VFS_PATH_MAX);

    *rc = norm ? vfs_normalize(path, norm, VFS_PATH_MAX) : -ENOMEM;
    if (*rc < 0) { kfree(norm); return NULL; }
    return norm;
}

/* ---- Mount table ---------------------------------------------------------- */

/* Longest mount point that is a path-component prefix of `path`; takes a ref. */
static mount_t *mount_get(const char *path, const char **rel) {
    mount_t *best = NULL;
    uintptr_t fl = spinlock_acquire_irqsave(&mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (!m->used || m->dying || strncmp(path, m->path, m->plen) != 0) continue;
        if (m->plen > 1 && path[m->plen] != '\0' && path[m->plen] != '/') continue;
        if (!best || m->plen > best->plen) best = m;
    }
    if (best) best->refs++;
    spinlock_release_irqrestore(&mount_lock, fl);
    if (best) {
        *rel = path + (best->plen > 1 ? best->plen : 0);
        if (**rel == '\0') *rel = "/";
    }
    return best;
}

static void mount_put(mount_t *m) {
    if (!m) return;                 /* anonymous objects (pipes) have no mount */
    uintptr_t fl = spinlock_acquire_irqsave(&mount_lock);
    m->refs--;
    spinlock_release_irqrestore(&mount_lock, fl);
}

int vfs_mount(const char *target, const char *fs_name, const char *device) {
    char path[MOUNT_PATH_MAX];
    int rc = vfs_normalize(target, path, sizeof(path));
    if (rc < 0) return rc;
    size_t plen = strlen(path);
    vfs_driver_t *drv = find_driver(fs_name);
    if (!drv || !drv->mount) return -ENODEV;

    uintptr_t fl = spinlock_acquire_irqsave(&mount_lock);
    mount_t *slot = NULL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && (strcmp(mounts[i].path, path) == 0 ||
                               (device && mounts[i].dev[0] && strcmp(mounts[i].dev, device) == 0))) {
            slot = NULL;            /* the mount point or the device is taken */
            rc = -EBUSY;
            break;
        }
        if (!mounts[i].used && !slot) slot = &mounts[i];
    }
    if (rc == 0 && !slot) rc = -ENOSPC;
    if (rc == 0) {                  /* reserved, but invisible to lookups until mounted */
        memcpy(slot->path, path, plen + 1);
        if (device) strncpy(slot->dev, device, sizeof(slot->dev) - 1);
        else slot->dev[0] = '\0';
        slot->plen = plen;
        slot->refs = 0;
        slot->used = true;
        slot->dying = true;
    }
    spinlock_release_irqrestore(&mount_lock, fl);
    if (rc < 0) return rc;

    void *ctx = drv->mount(device);
    if (!ctx) {
        slot->used = false;
        LOG_ERROR("mount %s on %s failed", fs_name, path);
        return -EINVAL;
    }
    slot->drv = drv;
    slot->ctx = ctx;
    slot->dying = false;
    LOG_INFO("mounted %s on %s", fs_name, path);
    return 0;
}

int vfs_umount(const char *target) {
    char path[MOUNT_PATH_MAX];
    int rc = vfs_normalize(target, path, sizeof(path));
    if (rc < 0) return rc;

    mount_t *m = NULL;
    uintptr_t fl = spinlock_acquire_irqsave(&mount_lock);
    rc = -EINVAL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_t *c = &mounts[i];
        if (!c->used || c->dying) continue;
        if (strcmp(c->path, path) == 0) { m = c; rc = 0; }
        else if (strncmp(c->path, path, strlen(path)) == 0 && (c->path[strlen(path)] == '/' || path[1] == '\0')) {
            rc = -EBUSY;            /* another mount sits below */
            m = NULL;
            break;
        }
    }
    if (m && m->refs > 0) rc = -EBUSY;
    if (rc == 0) m->dying = true;
    spinlock_release_irqrestore(&mount_lock, fl);
    if (rc < 0) return rc;

    if (m->drv->unmount) rc = m->drv->unmount(m->ctx);
    if (rc < 0) { m->dying = false; return rc; }
    m->used = false;
    return 0;
}

void vfs_sync(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (m->used && !m->dying && m->drv->sync) m->drv->sync(m->ctx);
    }
}

void vfs_shutdown(void) {
    vfs_sync();
    for (int i = MAX_MOUNTS - 1; i >= 0; i--) {
        if (mounts[i].used && !mounts[i].dying) vfs_umount(mounts[i].path);
    }
}

/* ---- Files ---------------------------------------------------------------- */

int vfs_open(const char *path, int flags, mode_t mode, file_t **out) {
    const char *rel;
    int rc;
    char *norm = norm_alloc(path, &rc);
    if (!norm) return rc;
    mount_t *m = mount_get(norm, &rel);
    if (!m) { kfree(norm); return -ENOENT; }

    vnode_t *v = kmalloc(sizeof(*v));
    file_t *f = kmalloc(sizeof(*f));
    if (!v || !f) { rc = -ENOMEM; goto fail_free; }
    memset(v, 0, sizeof(*v));
    v->fs_driver = m->drv;
    v->mnt = m;
    rc = m->drv->open(m->ctx, rel, flags, mode, v);
    if (rc < 0) goto fail_free;

    int acc = flags & O_ACCMODE;
    if (v->type == S_IFDIR && acc != O_RDONLY) rc = -EISDIR;
    else if ((flags & O_DIRECTORY) && v->type != S_IFDIR) rc = -ENOTDIR;
    else if ((flags & O_TRUNC) && acc != O_RDONLY && v->type == S_IFREG && m->drv->truncate)
        rc = m->drv->truncate(v, 0);
    if (rc < 0) { m->drv->close(v); goto fail_free; }

    f->vnode = v;
    f->flags = (uint32_t)flags;
    f->offset = 0;
    *out = f;
    kfree(norm);
    return 0;

fail_free:
    kfree(norm);
    kfree(v);
    kfree(f);
    mount_put(m);
    return rc;
}

int vfs_close(file_t *f) {
    vnode_t *v = f->vnode;
    int rc = v->fs_driver->close(v);
    mount_put(v->mnt);
    kfree(v);
    kfree(f);
    return rc;
}

ssize_t vfs_read(file_t *f, void *buf, size_t len) {
    vnode_t *v = f->vnode;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (v->type == S_IFDIR) return -EISDIR;
    if (!v->fs_driver->read) return -EINVAL;
    if (len > IO_MAX) len = IO_MAX;
    ssize_t n = v->fs_driver->read(v, f->offset, buf, len);
    if (n > 0) f->offset += n;
    return n;
}

ssize_t vfs_write(file_t *f, const void *buf, size_t len) {
    vnode_t *v = f->vnode;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (v->type == S_IFDIR) return -EISDIR;
    if (!v->fs_driver->write) return -EINVAL;
    if (f->flags & O_APPEND) {
        struct kstat64 st;
        int rc = vfs_fstat(f, &st);
        if (rc < 0) return rc;
        f->offset = (off_t)st.st_size;
    }
    if (len > IO_MAX) len = IO_MAX;
    if ((uint64_t)f->offset + len > 0x7fffffffu) {
        if (f->offset >= 0x7fffffff) return -EFBIG;
        len = 0x7fffffffu - (size_t)f->offset;
    }
    ssize_t n = v->fs_driver->write(v, f->offset, buf, len);
    if (n > 0) f->offset += n;
    return n;
}

off_t vfs_lseek(file_t *f, off_t off, int whence) {
    if (f->vnode->type != S_IFREG && f->vnode->type != S_IFDIR) return -ESPIPE;   /* devices, pipes, sockets */
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->offset; break;
    case SEEK_END: {
        struct kstat64 st;
        int rc = vfs_fstat(f, &st);
        if (rc < 0) return rc;
        base = st.st_size;
        break;
    }
    default: return -EINVAL;
    }
    int64_t pos = base + off;
    if (pos < 0) return -EINVAL;
    if (pos > 0x7fffffff) return -EOVERFLOW;
    f->offset = (off_t)pos;
    return f->offset;
}

int vfs_getdents64(file_t *f, void *buf, size_t len) {
    vnode_t *v = f->vnode;
    if (v->type != S_IFDIR || !v->fs_driver->readdir) return -ENOTDIR;
    struct vfs_dirent de;
    size_t used = 0;
    for (;;) {
        off_t next = f->offset;
        int rc = v->fs_driver->readdir(v, &next, &de);
        if (rc < 0) return used ? (int)used : rc;
        if (rc == 0) break;
        size_t nlen = strlen(de.name);
        size_t reclen = (sizeof(struct linux_dirent64) + nlen + 1 + 7) & ~(size_t)7;
        if (used + reclen > len) return used ? (int)used : -EINVAL;
        struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + used);
        d->d_ino = de.ino;
        d->d_off = next;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = de.type;
        memcpy(d->d_name, de.name, nlen + 1);
        used += reclen;
        f->offset = next;
    }
    return (int)used;
}

int vfs_fstat(file_t *f, struct kstat64 *st) {
    vnode_t *v = f->vnode;
    if (!v->fs_driver->getattr) return -ENOSYS;
    memset(st, 0, sizeof(*st));
    int rc = v->fs_driver->getattr(v, st);
    st->__st_ino = (uint32_t)st->st_ino;
    return rc;
}

int vfs_fstatfs(file_t *f, struct kstatfs64 *st) {
    vnode_t *v = f->vnode;
    int rc = 0;

    memset(st, 0, sizeof(*st));
    st->f_type = v->fs_driver->magic;
    st->f_bsize = st->f_frsize = 4096;
    st->f_namelen = VFS_NAME_MAX;
    if (v->mnt && v->mnt->ctx && v->fs_driver->statfs) rc = v->fs_driver->statfs(v->mnt->ctx, st);
    return rc;
}

int vfs_statfs(const char *path, struct kstatfs64 *st) {
    file_t *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) return rc;
    rc = vfs_fstatfs(f, st);
    vfs_close(f);
    return rc;
}

int vfs_ftruncate(file_t *f, off_t len) {
    vnode_t *v = f->vnode;
    if ((f->flags & O_ACCMODE) == O_RDONLY || len < 0) return -EINVAL;
    if (v->type == S_IFDIR) return -EISDIR;
    return v->fs_driver->truncate ? v->fs_driver->truncate(v, len) : -EINVAL;
}

int vfs_fsync(file_t *f) {
    vnode_t *v = f->vnode;
    if (v->fs_driver->fsync) return v->fs_driver->fsync(v);
    return v->fs_driver->sync ? v->fs_driver->sync(v->mnt->ctx) : 0;
}

uint32_t vfs_poll(file_t *f) {
    vfs_driver_t *d = f->vnode->fs_driver;
    return d->poll ? d->poll(f->vnode) : POLLIN | POLLOUT;
}

int vfs_ioctl(file_t *f, uint32_t cmd, void *arg) {
    vnode_t *v = f->vnode;
    return v->fs_driver->ioctl ? v->fs_driver->ioctl(v, cmd, arg) : -ENOTTY;
}

/* ---- Namespace operations ------------------------------------------------- */

int vfs_stat(const char *path, struct kstat64 *st) {
    file_t *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) return rc;
    rc = vfs_fstat(f, st);
    vfs_close(f);
    return rc;
}

int vfs_utime(const char *path, uint32_t atime, uint32_t mtime) {
    file_t *f;
    int rc = vfs_open(path, O_RDONLY, 0, &f);
    if (rc < 0) return rc;
    vfs_driver_t *d = f->vnode->fs_driver;
    rc = d->utime ? d->utime(f->vnode, atime, mtime) : -EROFS;
    vfs_close(f);
    return rc;
}

enum ns_op { NS_UNLINK, NS_MKDIR, NS_RMDIR };

static int ns_op(enum ns_op op, const char *path, mode_t mode) {
    const char *rel;
    int rc;
    char *norm = norm_alloc(path, &rc);
    if (!norm) return rc;
    mount_t *m = mount_get(norm, &rel);
    if (!m) { kfree(norm); return -ENOENT; }
    vfs_driver_t *d = m->drv;
    switch (op) {
    case NS_UNLINK: rc = d->unlink ? d->unlink(m->ctx, rel) : -EROFS; break;
    case NS_MKDIR:  rc = d->mkdir ? d->mkdir(m->ctx, rel, mode) : -EROFS; break;
    default:        rc = d->rmdir ? d->rmdir(m->ctx, rel) : -EROFS; break;
    }
    mount_put(m);
    kfree(norm);
    return rc;
}

int vfs_unlink(const char *path) { return ns_op(NS_UNLINK, path, 0); }
int vfs_mkdir(const char *path, mode_t mode) { return ns_op(NS_MKDIR, path, mode); }
int vfs_rmdir(const char *path) { return ns_op(NS_RMDIR, path, 0); }

int vfs_rename(const char *from, const char *to) {
    const char *ra, *rb;
    int rc;
    char *a = norm_alloc(from, &rc), *b = a ? norm_alloc(to, &rc) : NULL;
    if (!b) { kfree(a); return rc; }
    mount_t *ma = mount_get(a, &ra);
    mount_t *mb = mount_get(b, &rb);
    if (!ma || !mb) rc = -ENOENT;
    else if (ma != mb) rc = -EXDEV;
    else rc = ma->drv->rename ? ma->drv->rename(ma->ctx, ra, rb) : -EROFS;
    if (ma) mount_put(ma);
    if (mb) mount_put(mb);
    kfree(a);
    kfree(b);
    return rc;
}
