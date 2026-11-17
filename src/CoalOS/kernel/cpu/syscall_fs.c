/**
 * @file syscall_fs.c
 * @brief File descriptor and file system system calls.
 *
 * Descriptors point at refcounted sys_file_t objects shared by dup, dup2 and
 * fork. All I/O goes through the VFS, including the console on fds 0-2.
 */

#include "syscall_internal.h"
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/sync/spinlock.h>

#define IO_CHUNK        PAGE_SIZE
#define IOV_MAX         1024
#define LINUX_O_DIRECTORY 0200000

//============================================================================
// Descriptor table
//============================================================================

sys_file_t *fd_get(int fd) {
    pcb_t *p = get_current_process();
    sys_file_t *sf = NULL;

    if (fd < 0 || fd >= MAX_FD) return NULL;
    uintptr_t f = spinlock_acquire_irqsave(&p->fd_table_lock);
    if (p->fd_table[fd])
        sf = sys_file_get(p->fd_table[fd]);
    spinlock_release_irqrestore(&p->fd_table_lock, f);
    return sf;
}

/* Install sf (consuming the caller's reference) at the lowest free fd >= min. */
static long fd_install(sys_file_t *sf, int min) {
    pcb_t *p = get_current_process();

    uintptr_t f = spinlock_acquire_irqsave(&p->fd_table_lock);
    for (int fd = min; fd < MAX_FD; fd++) {
        if (!p->fd_table[fd]) {
            p->fd_table[fd] = sf;
            spinlock_release_irqrestore(&p->fd_table_lock, f);
            return fd;
        }
    }
    spinlock_release_irqrestore(&p->fd_table_lock, f);
    sys_file_put(sf);
    return -EMFILE;
}

long sc_close(uint32_t fd) {
    pcb_t *p = get_current_process();
    sys_file_t *sf = NULL;

    if (fd < MAX_FD) {
        uintptr_t f = spinlock_acquire_irqsave(&p->fd_table_lock);
        sf = p->fd_table[fd];
        p->fd_table[fd] = NULL;
        spinlock_release_irqrestore(&p->fd_table_lock, f);
    }
    return sf ? fs_ret(sys_file_put(sf)) : -EBADF;
}

long sc_dup(uint32_t fd) {
    sys_file_t *sf = fd_get(fd);
    return sf ? fd_install(sf, 0) : -EBADF;
}

long sc_dup2(uint32_t oldfd, uint32_t newfd) {
    pcb_t *p = get_current_process();
    sys_file_t *sf = fd_get(oldfd);

    if (!sf) return -EBADF;
    if (newfd >= MAX_FD) { sys_file_put(sf); return -EBADF; }
    if (oldfd == newfd) { sys_file_put(sf); return newfd; }

    uintptr_t f = spinlock_acquire_irqsave(&p->fd_table_lock);
    sys_file_t *old = p->fd_table[newfd];
    p->fd_table[newfd] = sf;                /* keeps the reference from fd_get */
    spinlock_release_irqrestore(&p->fd_table_lock, f);
    if (old) sys_file_put(old);
    return newfd;
}

//============================================================================
// open / read / write / seek
//============================================================================

static long open_path(const char *kpath, uint32_t flags) {
    file_t *file = vfs_open(kpath, flags & ~LINUX_O_CLOEXEC);
    if (!file)                              /* the VFS does not report why */
        return (flags & O_CREAT) && (flags & O_EXCL) ? -EEXIST : -ENOENT;

    sys_file_t *sf = sys_file_new(file, flags);
    if (!sf) { vfs_close(file); return -ENOMEM; }
    return fd_install(sf, 0);
}

long sc_open(uint32_t path, uint32_t flags, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = copy_path_from_user((const char *)path, kpath, sizeof(kpath));
    (void)mode;
    return err ? err : open_path(kpath, flags);
}

long sc_openat(int dirfd, uint32_t path, uint32_t flags, uint32_t mode) {
    if (dirfd != AT_FDCWD) return -ENOSYS;          /* directory-relative lookup is not supported */
    return sc_open(path, flags, mode);
}

/* One read or write of up to count bytes through a bounce buffer. */
static long transfer(uint32_t fd, uint32_t ubuf, uint32_t count, bool is_write) {
    sys_file_t *sf = fd_get(fd);
    long total = 0;

    if (!sf) return -EBADF;
    int mode = sf->flags & O_ACCMODE;
    if ((is_write && mode == O_RDONLY) || (!is_write && mode == O_WRONLY)) {
        sys_file_put(sf);
        return -EBADF;
    }
    if ((int32_t)count < 0) { sys_file_put(sf); return -EINVAL; }
    if (count == 0) { sys_file_put(sf); return 0; }

    size_t chunk = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(chunk);
    if (!kbuf) { sys_file_put(sf); return -ENOMEM; }

    while ((uint32_t)total < count) {
        size_t n = count - total < chunk ? count - total : chunk;
        long r;

        if (is_write) {
            if (copy_from_user(kbuf, (const char *)ubuf + total, n) != 0) { r = -EFAULT; goto out; }
            r = fs_ret(vfs_write(sf->vfs_file, kbuf, n));
        } else {
            r = fs_ret(vfs_read(sf->vfs_file, kbuf, n));
            if (r > 0 && copy_to_user((char *)ubuf + total, kbuf, r) != 0) r = -EFAULT;
        }
out:
        if (r < 0) { if (!total) total = r; break; }
        total += r;
        if ((size_t)r < n) break;                   /* short transfer: EOF, line, or full pipe */
    }
    kfree(kbuf);
    sys_file_put(sf);
    return total;
}

long sc_read(uint32_t fd, uint32_t buf, uint32_t count)  { return transfer(fd, buf, count, false); }
long sc_write(uint32_t fd, uint32_t buf, uint32_t count) { return transfer(fd, buf, count, true); }

/* Coal-only: the shell's line reader; same as read(0, ...). */
long sc_read_terminal_line(uint32_t buf, uint32_t count) { return transfer(0, buf, count, false); }

static long iov_transfer(uint32_t fd, uint32_t iov, uint32_t cnt, bool is_write) {
    long total = 0;

    if (cnt > IOV_MAX) return -EINVAL;
    for (uint32_t i = 0; i < cnt; i++) {
        struct linux_iovec v;
        if (copy_from_user(&v, (const struct linux_iovec *)iov + i, sizeof(v)) != 0)
            return total ? total : -EFAULT;
        long r = transfer(fd, v.iov_base, v.iov_len, is_write);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint32_t)r < v.iov_len) break;
    }
    return total;
}

long sc_readv(uint32_t fd, uint32_t iov, uint32_t cnt)  { return iov_transfer(fd, iov, cnt, false); }
long sc_writev(uint32_t fd, uint32_t iov, uint32_t cnt) { return iov_transfer(fd, iov, cnt, true); }

static long seek(uint32_t fd, int32_t off, uint32_t whence) {
    sys_file_t *sf = fd_get(fd);
    if (!sf) return -EBADF;
    if (whence > SEEK_END) { sys_file_put(sf); return -EINVAL; }
    long r = fs_ret(vfs_lseek(sf->vfs_file, off, whence));
    sys_file_put(sf);
    return r;
}

long sc_lseek(uint32_t fd, int32_t off, uint32_t whence) { return seek(fd, off, whence); }

/* _llseek: 64-bit offset in two halves, new position stored through result. */
long sc_llseek(uint32_t fd, uint32_t hi, uint32_t lo, uint32_t result, uint32_t whence) {
    if (hi != 0 && hi != 0xFFFFFFFFu) return -EOVERFLOW;    /* files are below 2 GiB */
    long pos = seek(fd, (int32_t)lo, whence);
    if (pos < 0) return pos;
    int64_t out = pos;
    return copy_to_user((void *)result, &out, sizeof(out)) ? -EFAULT : 0;
}

//============================================================================
// pipes, ioctl
//============================================================================

long sc_pipe2(uint32_t fds, uint32_t flags) {
    file_t *rd, *wr;
    int ends[2];

    if (flags & ~(uint32_t)(LINUX_O_CLOEXEC | 04000)) return -EINVAL;    /* O_NONBLOCK is accepted by the VFS */
    int err = vfs_pipe(&rd, &wr);
    if (err) return fs_ret(err);

    sys_file_t *r = sys_file_new(rd, O_RDONLY | (flags & 04000));
    sys_file_t *w = sys_file_new(wr, O_WRONLY | (flags & 04000));
    if (!r || !w) {
        if (r) sys_file_put(r); else vfs_close(rd);
        if (w) sys_file_put(w); else vfs_close(wr);
        return -ENOMEM;
    }
    long fr = fd_install(r, 0);
    if (fr < 0) { sys_file_put(w); return fr; }
    long fw = fd_install(w, 0);
    if (fw < 0) { sc_close(fr); return fw; }

    ends[0] = fr; ends[1] = fw;
    if (copy_to_user((void *)fds, ends, sizeof(ends)) != 0) {
        sc_close(fr); sc_close(fw);
        return -EFAULT;
    }
    return 0;
}

long sc_pipe(uint32_t fds) { return sc_pipe2(fds, 0); }

static bool is_console(const file_t *f) {
    return f->vnode && f->vnode->fs_driver && f->vnode->fs_driver->fs_name &&
           strcmp(f->vnode->fs_driver->fs_name, "console") == 0;
}

/* Just enough terminal control for isatty() and window size queries. */
long sc_ioctl(uint32_t fd, uint32_t cmd, uint32_t arg) {
    sys_file_t *sf = fd_get(fd);
    long r = -ENOTTY;

    if (!sf) return -EBADF;
    if (is_console(sf->vfs_file)) {
        if (cmd == TCGETS) {
            struct linux_termios t = {
                .c_iflag = 0x0500,      /* ICRNL | IXON */
                .c_oflag = 0x0005,      /* OPOST | ONLCR */
                .c_cflag = 0x00BF,      /* B38400 | CS8 | CREAD | HUPCL */
                .c_lflag = 0x803B,      /* ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN */
                .c_cc = { 3, 28, 127, 21, 4, 0, 1 },   /* INTR QUIT ERASE KILL EOF TIME MIN */
            };
            r = copy_to_user((void *)arg, &t, sizeof(t)) ? -EFAULT : 0;
        } else if (cmd == TIOCGWINSZ) {
            struct linux_winsize w = { .ws_row = VGA_ROWS, .ws_col = VGA_COLS };
            r = copy_to_user((void *)arg, &w, sizeof(w)) ? -EFAULT : 0;
        } else {
            r = -EINVAL;
        }
    }
    sys_file_put(sf);
    return r;
}

//============================================================================
// Path operations
//============================================================================

static long path_op(uint32_t upath, int (*op)(const char *)) {
    char kpath[PATH_MAX_SYS];
    int err = copy_path_from_user((const char *)upath, kpath, sizeof(kpath));
    return err ? err : fs_ret(op(kpath));
}

long sc_unlink(uint32_t path) { return path_op(path, vfs_unlink); }

static int rmdir_op(const char *p) {
    int r = vfs_rmdir(p);
    return r == -FS_ERR_BUSY ? -ENOTEMPTY : r;      /* the FS reports a non-empty directory as busy */
}
long sc_rmdir(uint32_t path) { return path_op(path, rmdir_op); }

long sc_mkdir(uint32_t path, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = copy_path_from_user((const char *)path, kpath, sizeof(kpath));
    return err ? err : fs_ret(vfs_mkdir(kpath, mode));
}

long sc_chdir(uint32_t path) { return path_op(path, process_chdir); }

long sc_getcwd(uint32_t buf, uint32_t size) {
    char kbuf[PATH_MAX_SYS];
    if (size > sizeof(kbuf)) size = sizeof(kbuf);
    int len = process_getcwd(kbuf, size);
    if (len < 0) return fs_ret(len);
    if (len > (int)size) return -ERANGE;
    return copy_to_user((void *)buf, kbuf, len) ? -EFAULT : len;   /* Linux returns the length */
}

long sc_access(uint32_t path, uint32_t mode) {
    struct linux_stat64 st;
    char kpath[PATH_MAX_SYS];
    int err = copy_path_from_user((const char *)path, kpath, sizeof(kpath));
    (void)mode;                                     /* single user, no permission bits enforced */
    return err ? err : fs_ret(vfs_stat64(kpath, &st));
}

static long put_stat(uint32_t ubuf, int fs_err, const struct linux_stat64 *st) {
    if (fs_err) return fs_ret(fs_err);
    return copy_to_user((void *)ubuf, st, sizeof(*st)) ? -EFAULT : 0;
}

long sc_stat64(uint32_t path, uint32_t buf) {
    struct linux_stat64 st;
    char kpath[PATH_MAX_SYS];
    int err = copy_path_from_user((const char *)path, kpath, sizeof(kpath));
    if (err) return err;
    memset(&st, 0, sizeof(st));
    return put_stat(buf, vfs_stat64(kpath, &st), &st);
}

long sc_fstat64(uint32_t fd, uint32_t buf) {
    struct linux_stat64 st;
    sys_file_t *sf = fd_get(fd);
    if (!sf) return -EBADF;
    memset(&st, 0, sizeof(st));
    long r = put_stat(buf, vfs_fstat64(sf->vfs_file, &st), &st);
    sys_file_put(sf);
    return r;
}

long sc_fstatat64(int dirfd, uint32_t path, uint32_t buf, uint32_t flags) {
    (void)flags;                                    /* AT_SYMLINK_NOFOLLOW: no symbolic links */
    if (dirfd != AT_FDCWD) return -ENOSYS;
    return sc_stat64(path, buf);
}

long sc_getdents64(uint32_t fd, uint32_t buf, uint32_t count) {
    sys_file_t *sf = fd_get(fd);
    if (!sf) return -EBADF;

    size_t len = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(len ? len : 1);
    long r;
    if (!kbuf) {
        r = -ENOMEM;
    } else {
        r = fs_ret(vfs_getdents64(sf->vfs_file, kbuf, len));
        if (r > 0 && copy_to_user((void *)buf, kbuf, r) != 0) r = -EFAULT;
        kfree(kbuf);
    }
    sys_file_put(sf);
    return r;
}
