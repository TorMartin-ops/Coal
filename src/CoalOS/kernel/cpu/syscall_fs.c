/**
 * @file syscall_fs.c
 * @brief File descriptor and file system system calls.
 *
 * Thin layer over sys_file.h / vfs.h: copies arguments across the user
 * boundary, resolves paths against the process cwd and forwards. The FS code
 * already returns negative Linux errno values.
 */

#include "syscall_internal.h"
#include <kernel/memory/kmalloc.h>
#include <kernel/process/scheduler.h>
#include <kernel/lib/string.h>
#include <kernel/fs/vfs/fs_init.h>

#define IO_CHUNK    PAGE_SIZE
#define IOV_MAX     1024
#define F_DUPFD_CLOEXEC 1030
#define LINUX_O_LARGEFILE 0x8000
#define MAX_RW_COUNT    0x7ffff000ul    /* what Linux clamps a single read or write to */

_Static_assert(sizeof(struct kstat) == 144 && sizeof(struct kstatfs) == 120, "x86_64 struct stat and statfs sizes");

static void set_cloexec(int fd, bool on) {
    pcb_t *p = get_current_process();
    if (fd < 0 || fd >= MAX_FD) return;
    if (on) p->cloexec |= 1u << fd; else p->cloexec &= ~(1u << fd);
}

//============================================================================
// Descriptors
//============================================================================

long sc_open(uintptr_t path, uint32_t flags, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    if (err) return err;

    int fd = sys_open(kpath, flags & ~LINUX_O_CLOEXEC, mode);
    if (fd >= 0) set_cloexec(fd, flags & LINUX_O_CLOEXEC);
    return fd;
}

long sc_openat(int dirfd, uintptr_t path, uint32_t flags, uint32_t mode) {
    if (dirfd != AT_FDCWD) return -ENOSYS;          /* directory-relative lookup is not supported */
    return sc_open(path, flags, mode);
}

long sc_creat(uintptr_t path, uint32_t mode) { return sc_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode); }

long sc_close(uint32_t fd) {
    set_cloexec(fd, false);
    return sys_close(fd);
}

long sc_dup3(uint32_t oldfd, uint32_t newfd, uint32_t flags) {
    if (flags & ~(uint32_t)LINUX_O_CLOEXEC || oldfd == newfd) return -EINVAL;
    long r = sc_dup2(oldfd, newfd);
    if (r >= 0) set_cloexec(r, flags & LINUX_O_CLOEXEC);
    return r;
}

long sc_dup(uint32_t fd) { return sys_dup(fd); }

long sc_dup2(uint32_t oldfd, uint32_t newfd) {
    long r = sys_dup2(oldfd, newfd);
    if (r >= 0) set_cloexec(r, false);
    return r;
}

/* F_DUPFD: lowest free descriptor >= min. */
static long dup_from(uint32_t fd, uint32_t min, bool cloexec) {
    pcb_t *p = get_current_process();
    sys_file_t *sf = sys_file_get(fd);      /* the reference moves into the table */

    if (!sf) return -EBADF;
    if (min >= MAX_FD) { sys_file_put(sf); return -EINVAL; }
    uintptr_t f = spinlock_acquire_irqsave(&p->fd_table_lock);
    for (uint32_t i = min; i < MAX_FD; i++) {
        if (!p->fd_table[i]) {
            p->fd_table[i] = sf;
            spinlock_release_irqrestore(&p->fd_table_lock, f);
            set_cloexec(i, cloexec);
            return i;
        }
    }
    spinlock_release_irqrestore(&p->fd_table_lock, f);
    sys_file_put(sf);
    return -EMFILE;
}

long sc_fcntl(uint32_t fd, uint32_t cmd, unsigned long arg) {
    sys_file_t *sf;
    long r;

    switch (cmd) {
    case F_DUPFD:         return dup_from(fd, arg, false);
    case F_DUPFD_CLOEXEC: return dup_from(fd, arg, true);
    case F_GETFD:
    case F_GETFL:
    case F_SETFL:
        if (!(sf = sys_file_get(fd))) return -EBADF;
        if (cmd == F_GETFD) {
            r = (get_current_process()->cloexec >> fd) & 1 ? FD_CLOEXEC : 0;
        } else if (cmd == F_GETFL) {
            r = (sf->flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY)) | LINUX_O_LARGEFILE;
        } else {
            sf->flags = (sf->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK));
            if (sf->vfs_file->vnode->fs_driver->set_nonblock)       /* drivers that block themselves (sockets) */
                sf->vfs_file->vnode->fs_driver->set_nonblock(sf->vfs_file->vnode, (arg & O_NONBLOCK) != 0);
            r = 0;
        }
        sys_file_put(sf);
        return r;
    case F_SETFD:
        if (!(sf = sys_file_get(fd))) return -EBADF;
        sys_file_put(sf);
        set_cloexec(fd, arg & FD_CLOEXEC);
        return 0;
    default:
        return -EINVAL;
    }
}

long sc_pipe2(uintptr_t ufds, uint32_t flags) {
    int fds[2];

    if (flags & ~(uint32_t)(LINUX_O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    int err = sys_pipe(fds, flags & O_NONBLOCK);
    if (err) return err;
    if (copy_to_user((void *)ufds, fds, sizeof(fds)) != 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return -EFAULT;
    }
    set_cloexec(fds[0], flags & LINUX_O_CLOEXEC);
    set_cloexec(fds[1], flags & LINUX_O_CLOEXEC);
    return 0;
}

long sc_pipe(uintptr_t fds) { return sc_pipe2(fds, 0); }

/* The driver behind the descriptor reads and writes the user argument itself. */
long sc_ioctl(uint32_t fd, uint32_t cmd, uintptr_t arg) {
    return sys_ioctl(fd, cmd, (void *)arg);
}

//============================================================================
// read / write / seek
//============================================================================

static bool regular_file(uint32_t fd) {
    sys_file_t *sf = sys_file_get(fd);
    bool reg = sf && S_ISREG(sf->vfs_file->vnode->type);

    if (sf) sys_file_put(sf);
    return reg;
}

/* One read or write of up to count bytes through a bounce buffer. */
static long transfer(uint32_t fd, uintptr_t ubuf, size_t count, bool is_write) {
    long total = 0;

    if (count > MAX_RW_COUNT) count = MAX_RW_COUNT;
    if (count == 0) return 0;

    size_t chunk = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(chunk);
    if (!kbuf) return -ENOMEM;

    while ((size_t)total < count) {
        size_t n = count - total < chunk ? count - total : chunk;
        long r;

        if (is_write) {
            r = copy_from_user(kbuf, (const char *)ubuf + total, n) ? -EFAULT : sys_write(fd, kbuf, n);
        } else {
            r = sys_read(fd, kbuf, n);
            if (r > 0 && copy_to_user((char *)ubuf + total, kbuf, r) != 0) r = -EFAULT;
        }
        if (r < 0) { if (!total) total = r; break; }
        total += r;
        if ((size_t)r < n) break;                   /* short transfer: EOF, line, or full pipe */
        if (!is_write && (size_t)total < count && !regular_file(fd)) break;   /* pipes, ttys, sockets: no refill */
    }
    kfree(kbuf);
    return total;
}

long sc_read(uint32_t fd, uintptr_t buf, size_t count)  { return transfer(fd, buf, count, false); }
long sc_write(uint32_t fd, uintptr_t buf, size_t count) { return transfer(fd, buf, count, true); }

static long iov_transfer(uint32_t fd, uintptr_t iov, unsigned long cnt, bool is_write) {
    long total = 0;

    if (cnt > IOV_MAX) return -EINVAL;
    for (unsigned long i = 0; i < cnt; i++) {
        struct linux_iovec v;
        if (copy_from_user(&v, (const struct linux_iovec *)iov + i, sizeof(v)) != 0)
            return total ? total : -EFAULT;
        long r = transfer(fd, v.iov_base, v.iov_len, is_write);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < v.iov_len) break;
    }
    return total;
}

long sc_readv(uint32_t fd, uintptr_t iov, unsigned long cnt)  { return iov_transfer(fd, iov, cnt, false); }
long sc_writev(uint32_t fd, uintptr_t iov, unsigned long cnt) { return iov_transfer(fd, iov, cnt, true); }

/* pread64/pwrite64: the transfer runs at `pos` and leaves the file offset alone. */
static long positional(uint32_t fd, uintptr_t ubuf, size_t count, int64_t pos, bool is_write) {
    sys_file_t *sf = sys_file_get(fd);
    long r;

    if (!sf) return -EBADF;
    file_t *f = sf->vfs_file;
    if (!S_ISREG(f->vnode->type)) r = -ESPIPE;
    else if (pos < 0) r = -EINVAL;
    else {
        off_t saved = f->offset;

        f->offset = pos;
        r = transfer(fd, ubuf, count, is_write);
        f->offset = saved;
    }
    sys_file_put(sf);
    return r;
}

long sc_pread64(uint32_t fd, uintptr_t buf, size_t count, int64_t pos)  { return positional(fd, buf, count, pos, false); }
long sc_pwrite64(uint32_t fd, uintptr_t buf, size_t count, int64_t pos) { return positional(fd, buf, count, pos, true); }

long sc_lseek(uint32_t fd, int64_t off, uint32_t whence) {
    return sys_lseek(fd, off, whence);
}

//============================================================================
// Paths
//============================================================================

static long path_op(uintptr_t upath, int (*op)(const char *)) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(upath, kpath, sizeof(kpath));
    return err ? err : op(kpath);
}

long sc_unlink(uintptr_t path) { return path_op(path, vfs_unlink); }
long sc_rmdir(uintptr_t path)  { return path_op(path, vfs_rmdir); }

long sc_mkdir(uintptr_t path, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    return err ? err : vfs_mkdir(kpath, mode & 07777);
}

/* Two path buffers on the heap: PATH_MAX is too large for two on the kernel stack. */
long sc_rename(uintptr_t from, uintptr_t to) {
    char *a = kmalloc(2 * PATH_MAX_SYS), *b = a + PATH_MAX_SYS;
    int err;

    if (!a) return -ENOMEM;
    err = user_path(from, a, PATH_MAX_SYS);
    if (!err) err = user_path(to, b, PATH_MAX_SYS);
    if (!err) err = vfs_rename(a, b);
    kfree(a);
    return err;
}

long sc_utime(uintptr_t path, uintptr_t utimes) {
    char kpath[PATH_MAX_SYS];
    int64_t t[2];
    int err = user_path(path, kpath, sizeof(kpath));

    if (err) return err;
    if (utimes) {
        if (copy_from_user(t, (const void *)utimes, sizeof(t)) != 0) return -EFAULT;
    } else {
        t[0] = t[1] = vfs_time();
    }
    return vfs_utime(kpath, (uint32_t)t[0], (uint32_t)t[1]);
}

long sc_truncate(uintptr_t path, int64_t len) {
    char kpath[PATH_MAX_SYS];
    file_t *f;
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_open(kpath, O_WRONLY, 0, &f);
    if (err) return err;
    err = vfs_ftruncate(f, len);
    vfs_close(f);
    return err;
}

long sc_ftruncate(uint32_t fd, int64_t len) {
    sys_file_t *sf = sys_file_get(fd);

    if (!sf) return -EBADF;
    long r = vfs_ftruncate(sf->vfs_file, len);
    sys_file_put(sf);
    return r;
}

/* Every file belongs to root and FAT stores no owner. Like vfat, chown succeeds only if nothing would change. */
static long chown_result(long err, uint32_t uid, uint32_t gid) {
    if (err) return err;
    return (uid == 0 || uid == (uint32_t)-1) && (gid == 0 || gid == (uint32_t)-1) ? 0 : -EPERM;
}

long sc_chown(uintptr_t path, uint32_t uid, uint32_t gid) {
    struct kstat st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));

    return chown_result(err ? err : vfs_stat(kpath, &st), uid, gid);
}

long sc_fchown(uint32_t fd, uint32_t uid, uint32_t gid) {
    struct kstat st;

    return chown_result(sys_fstat(fd, &st), uid, gid);
}

long sc_umask(uint32_t mask) {
    pcb_t *p = get_current_process();
    uint32_t old = p->umask;

    p->umask = mask & 0777;
    return old;
}

long sc_sync(void) {
    vfs_sync();
    return 0;
}

/* mount(source, target, fstype): "fat" mounts an ATA disk given as "hdb" or "/dev/hdb";
 * the root disk is already mounted (-EBUSY). devfs and procfs take no device. */
long sc_fsync(uint32_t fd) {
    sys_file_t *sf = sys_file_get(fd);
    if (!sf) return -EBADF;
    long rc = vfs_fsync(sf->vfs_file);
    sys_file_put(sf);
    return rc;
}

long sc_mount(uintptr_t source, uintptr_t target, uintptr_t fstype) {
    char dev[16], fs[16];
    const char *name = NULL;
    char *dir = kmalloc(PATH_MAX_SYS);
    int err;

    if (!dir) return -ENOMEM;
    err = fstype ? 0 : -EINVAL;
    if (!err) err = copy_path_from_user((const char *)fstype, fs, sizeof(fs));
    if (!err) err = user_path(target, dir, PATH_MAX_SYS);
    if (!err) {
        struct kstat st;
        err = vfs_stat(dir, &st);
        if (!err && !S_ISDIR(st.st_mode)) err = -ENOTDIR;
    }
    if (!err && strcmp(fs, "fat") == 0) {
        err = copy_path_from_user((const char *)source, dev, sizeof(dev));
        name = strncmp(dev, "/dev/", 5) == 0 ? dev + 5 : dev;
        if (!err) err = fs_attach_disk(name);
    }
    if (!err) err = vfs_mount(dir, fs, name);
    kfree(dir);
    return err;
}

long sc_umount2(uintptr_t target, uint32_t flags) {
    (void)flags;
    return path_op(target, vfs_umount);
}

long sc_chdir(uintptr_t path) {
    char kpath[PATH_MAX_SYS];
    struct kstat st;
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_stat(kpath, &st);
    if (err) return err;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (strlen(kpath) >= CWD_MAX) return -ENAMETOOLONG;
    strcpy(get_current_process()->cwd, kpath);
    return 0;
}

long sc_getcwd(uintptr_t buf, size_t size) {
    const char *cwd = get_current_process()->cwd;
    size_t len = strlen(cwd) + 1;

    if (size < len) return -ERANGE;
    return copy_to_user((void *)buf, cwd, len) ? -EFAULT : (long)len;   /* Linux returns the length */
}

long sc_access(uintptr_t path, uint32_t mode) {
    struct kstat st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    if (mode & ~(uint32_t)7) return -EINVAL;
    return err ? err : vfs_stat(kpath, &st);        /* single user, no permission bits enforced */
}

//============================================================================
// stat, directories
//============================================================================

long sc_stat(uintptr_t path, uintptr_t buf) {
    struct kstat st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_stat(kpath, &st);
    if (err) return err;
    return copy_to_user((void *)buf, &st, sizeof(st)) ? -EFAULT : 0;
}

long sc_fstat(uint32_t fd, uintptr_t buf) {
    struct kstat st;
    int err = sys_fstat(fd, &st);
    if (err) return err;
    return copy_to_user((void *)buf, &st, sizeof(st)) ? -EFAULT : 0;
}

long sc_newfstatat(int dirfd, uintptr_t path, uintptr_t buf, uint32_t flags) {
    char first;

    if ((flags & AT_EMPTY_PATH) && path && copy_from_user(&first, (const void *)path, 1) == 0 && !first)
        return sc_fstat(dirfd, buf);                /* fstat as newer libcs spell it */
    if (dirfd != AT_FDCWD) return -ENOSYS;          /* AT_SYMLINK_NOFOLLOW is moot: no symbolic links */
    return sc_stat(path, buf);
}

static long put_statfs(uintptr_t buf, const struct kstatfs *st) {
    return copy_to_user((void *)buf, st, sizeof(*st)) ? -EFAULT : 0;
}

static long statfs_path(uintptr_t path, uintptr_t buf) {
    struct kstatfs st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_statfs(kpath, &st);
    return err ? err : put_statfs(buf, &st);
}

static long statfs_fd(uint32_t fd, uintptr_t buf) {
    struct kstatfs st;
    int err = sys_fstatfs((int)fd, &st);
    return err ? err : put_statfs(buf, &st);
}

long sc_statfs(uintptr_t path, uintptr_t buf) { return statfs_path(path, buf); }
long sc_fstatfs(uint32_t fd, uintptr_t buf) { return statfs_fd(fd, buf); }

/* FAT has no hard or symbolic links, which is how Linux's vfat answers too. */
long sc_link(uintptr_t from, uintptr_t to) {
    struct kstat st;
    char a[PATH_MAX_SYS], b[PATH_MAX_SYS];
    int err = user_path(from, a, sizeof(a));

    if (!err) err = user_path(to, b, sizeof(b));
    if (!err) err = vfs_stat(a, &st);
    return err ? err : -EPERM;
}

long sc_symlink(uintptr_t target, uintptr_t linkpath) {
    char a[PATH_MAX_SYS], b[PATH_MAX_SYS];
    int err = user_path(target, a, sizeof(a));

    if (!err) err = user_path(linkpath, b, sizeof(b));
    return err ? err : vfs_stat(b, &(struct kstat){ 0 }) == 0 ? -EEXIST : -EPERM;
}

/* Nothing here is a symbolic link: existing paths are "not a link", the rest do not exist. */
long sc_readlink(uintptr_t path, uintptr_t buf, long size) {
    struct kstat st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));

    (void)buf;
    if (!err) err = size > 0 ? vfs_stat(kpath, &st) : -EINVAL;
    return err ? err : -EINVAL;
}

long sc_getdents64(uint32_t fd, uintptr_t buf, size_t count) {
    size_t len = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(len ? len : 1);

    if (!kbuf) return -ENOMEM;
    long r = sys_getdents64(fd, kbuf, len);
    if (r > 0 && copy_to_user((void *)buf, kbuf, r) != 0) r = -EFAULT;
    kfree(kbuf);
    return r;
}
