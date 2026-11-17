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
#include <kernel/drivers/wait.h>
#include <kernel/drivers/timer/pit.h>

#define IO_CHUNK    PAGE_SIZE
#define IOV_MAX     1024
#define F_DUPFD_CLOEXEC 1030

_Static_assert(sizeof(struct kstat64) == 96, "struct stat64 is 96 bytes on i386");

static void set_cloexec(int fd, bool on) {
    pcb_t *p = get_current_process();
    if (fd < 0 || fd >= MAX_FD) return;
    if (on) p->cloexec |= 1u << fd; else p->cloexec &= ~(1u << fd);
}

//============================================================================
// Descriptors
//============================================================================

long sc_open(uint32_t path, uint32_t flags, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    if (err) return err;

    int fd = sys_open(kpath, flags & ~LINUX_O_CLOEXEC, mode);
    if (fd >= 0) set_cloexec(fd, flags & LINUX_O_CLOEXEC);
    return fd;
}

long sc_openat(int dirfd, uint32_t path, uint32_t flags, uint32_t mode) {
    if (dirfd != AT_FDCWD) return -ENOSYS;          /* directory-relative lookup is not supported */
    return sc_open(path, flags, mode);
}

long sc_close(uint32_t fd) {
    set_cloexec(fd, false);
    return sys_close(fd);
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

long sc_fcntl64(uint32_t fd, uint32_t cmd, uint32_t arg) {
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
            r = sf->flags;
        } else {
            sf->flags = (sf->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK));
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

long sc_pipe2(uint32_t ufds, uint32_t flags) {
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

long sc_pipe(uint32_t fds) { return sc_pipe2(fds, 0); }

/* The driver behind the descriptor reads and writes the user argument itself. */
long sc_ioctl(uint32_t fd, uint32_t cmd, uint32_t arg) {
    return sys_ioctl(fd, cmd, (void *)arg);
}

#define POLL_MAX 128

/* Readiness of one descriptor; call with IRQs off so no wakeup can slip in before the task blocks. */
static int16_t poll_one(const struct linux_pollfd *p) {
    if (p->fd < 0) return 0;
    sys_file_t *sf = sys_file_get(p->fd);
    if (!sf) return POLLNVAL;
    uint32_t r = vfs_poll(sf->vfs_file);
    sys_file_put(sf);
    return (int16_t)(r & (p->events | POLLERR | POLLHUP));
}

long sc_poll(uint32_t ufds, uint32_t nfds, int32_t timeout_ms) {
    struct linux_pollfd fds[POLL_MAX];
    uint64_t deadline = timeout_ms > 0 ? timer_uptime_ms() + (uint32_t)timeout_ms : 0;
    long ready;

    if (nfds > POLL_MAX) return -EINVAL;
    if (copy_from_user(fds, (const void *)ufds, nfds * sizeof(fds[0])) != 0) return -EFAULT;
    for (;;) {
        uintptr_t flags = local_irq_save();
        ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = poll_one(&fds[i]);
            ready += fds[i].revents != 0;
        }
        if (!ready && timeout_ms != 0) {
            uint64_t now = timer_uptime_ms();
            if (timeout_ms < 0 || now < deadline) {
                if (get_current_process()->pending_signals & ~get_current_process()->sig_blocked) {
                    local_irq_restore(flags);
                    return -EINTR;
                }
                if (timeout_ms < 0) drv_wait(&drv_poll_queue);
                else drv_wait_timeout(&drv_poll_queue, (uint32_t)(deadline - now));
                local_irq_restore(flags);
                continue;
            }
        }
        local_irq_restore(flags);
        break;
    }
    for (uint32_t i = 0; i < nfds; i++)
        if (copy_to_user(&((struct linux_pollfd *)ufds)[i].revents, &fds[i].revents, sizeof(int16_t)) != 0)
            return -EFAULT;
    return ready;
}

//============================================================================
// read / write / seek
//============================================================================

/* One read or write of up to count bytes through a bounce buffer. */
static long transfer(uint32_t fd, uint32_t ubuf, uint32_t count, bool is_write) {
    long total = 0;

    if ((int32_t)count < 0) return -EINVAL;
    if (count == 0) return 0;

    size_t chunk = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(chunk);
    if (!kbuf) return -ENOMEM;

    while ((uint32_t)total < count) {
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
    }
    kfree(kbuf);
    return total;
}

long sc_read(uint32_t fd, uint32_t buf, uint32_t count)  { return transfer(fd, buf, count, false); }
long sc_write(uint32_t fd, uint32_t buf, uint32_t count) { return transfer(fd, buf, count, true); }

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

long sc_lseek(uint32_t fd, int32_t off, uint32_t whence) {
    return sys_lseek(fd, off, whence);
}

/* _llseek: 64-bit offset in two halves, new position stored through result. */
long sc_llseek(uint32_t fd, uint32_t hi, uint32_t lo, uint32_t result, uint32_t whence) {
    if (hi != 0 && hi != 0xFFFFFFFFu) return -EOVERFLOW;    /* files are below 2 GiB */
    long pos = sys_lseek(fd, (int32_t)lo, whence);
    if (pos < 0) return pos;
    int64_t out = pos;
    return copy_to_user((void *)result, &out, sizeof(out)) ? -EFAULT : 0;
}

//============================================================================
// Paths
//============================================================================

static long path_op(uint32_t upath, int (*op)(const char *)) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(upath, kpath, sizeof(kpath));
    return err ? err : op(kpath);
}

long sc_unlink(uint32_t path) { return path_op(path, vfs_unlink); }
long sc_rmdir(uint32_t path)  { return path_op(path, vfs_rmdir); }

long sc_mkdir(uint32_t path, uint32_t mode) {
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    return err ? err : vfs_mkdir(kpath, mode & 07777);
}

long sc_rename(uint32_t from, uint32_t to) {
    char a[PATH_MAX_SYS], b[PATH_MAX_SYS];
    int err = user_path(from, a, sizeof(a));
    if (!err) err = user_path(to, b, sizeof(b));
    return err ? err : vfs_rename(a, b);
}

long sc_utime(uint32_t path, uint32_t utimes) {
    char kpath[PATH_MAX_SYS];
    uint32_t t[2];
    int err = user_path(path, kpath, sizeof(kpath));

    if (err) return err;
    if (utimes) {
        if (copy_from_user(t, (const void *)utimes, sizeof(t)) != 0) return -EFAULT;
    } else {
        t[0] = t[1] = vfs_time();
    }
    return vfs_utime(kpath, t[0], t[1]);
}

long sc_sync(void) {
    vfs_sync();
    return 0;
}

long sc_mount(uint32_t source, uint32_t target, uint32_t fstype) {
    char dev[PATH_MAX_SYS], dir[PATH_MAX_SYS], fs[32];
    int err = user_path(source, dev, sizeof(dev));

    if (!err) err = user_path(target, dir, sizeof(dir));
    if (!err) err = copy_path_from_user((const char *)fstype, fs, sizeof(fs));
    return err ? err : vfs_mount(dir, fs, dev);
}

long sc_umount2(uint32_t target, uint32_t flags) {
    (void)flags;
    return path_op(target, vfs_umount);
}

long sc_chdir(uint32_t path) {
    char kpath[PATH_MAX_SYS];
    struct kstat64 st;
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_stat(kpath, &st);
    if (err) return err;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    if (strlen(kpath) >= CWD_MAX) return -ENAMETOOLONG;
    strcpy(get_current_process()->cwd, kpath);
    return 0;
}

long sc_getcwd(uint32_t buf, uint32_t size) {
    const char *cwd = get_current_process()->cwd;
    size_t len = strlen(cwd) + 1;

    if (size < len) return -ERANGE;
    return copy_to_user((void *)buf, cwd, len) ? -EFAULT : (long)len;   /* Linux returns the length */
}

long sc_access(uint32_t path, uint32_t mode) {
    struct kstat64 st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));
    (void)mode;                                     /* single user, no permission bits enforced */
    return err ? err : vfs_stat(kpath, &st);
}

//============================================================================
// stat, directories
//============================================================================

long sc_stat64(uint32_t path, uint32_t buf) {
    struct kstat64 st;
    char kpath[PATH_MAX_SYS];
    int err = user_path(path, kpath, sizeof(kpath));

    if (!err) err = vfs_stat(kpath, &st);
    if (err) return err;
    return copy_to_user((void *)buf, &st, sizeof(st)) ? -EFAULT : 0;
}

long sc_fstat64(uint32_t fd, uint32_t buf) {
    struct kstat64 st;
    int err = sys_fstat(fd, &st);
    if (err) return err;
    return copy_to_user((void *)buf, &st, sizeof(st)) ? -EFAULT : 0;
}

long sc_fstatat64(int dirfd, uint32_t path, uint32_t buf, uint32_t flags) {
    (void)flags;                                    /* AT_SYMLINK_NOFOLLOW: no symbolic links */
    if (dirfd != AT_FDCWD) return -ENOSYS;
    return sc_stat64(path, buf);
}

long sc_getdents64(uint32_t fd, uint32_t buf, uint32_t count) {
    size_t len = count < IO_CHUNK ? count : IO_CHUNK;
    char *kbuf = kmalloc(len ? len : 1);

    if (!kbuf) return -ENOMEM;
    long r = sys_getdents64(fd, kbuf, len);
    if (r > 0 && copy_to_user((void *)buf, kbuf, r) != 0) r = -EFAULT;
    kfree(kbuf);
    return r;
}
