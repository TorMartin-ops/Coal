/**
 * @file sys_file.c
 * @brief File descriptor table operations (open/read/write/close/dup/...).
 *
 * A descriptor slot owns one reference to a sys_file_t; operations take their
 * own reference while they run so a concurrent close cannot free the file.
 */
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/process/process.h>
#include <kernel/sync/spinlock.h>

sys_file_t *sys_file_ref(sys_file_t *sf) {
    sf->dups++;
    return sf;
}

int sys_file_put(sys_file_t *sf) {
    if (sf->dups > 0) { sf->dups--; return 0; }
    int rc = vfs_close(sf->vfs_file);
    kfree(sf);
    return rc;
}

sys_file_t *sys_file_get(int fd) {
    pcb_t *p = get_current_process();
    if (!p || fd < 0 || fd >= MAX_FD) return NULL;
    uintptr_t fl = spinlock_acquire_irqsave(&p->fd_table_lock);
    sys_file_t *sf = p->fd_table[fd];
    if (sf) sys_file_ref(sf);
    spinlock_release_irqrestore(&p->fd_table_lock, fl);
    return sf;
}

/* Install sf in the lowest free slot; the caller's reference moves to the table. */
static int fd_install(pcb_t *p, sys_file_t *sf) {
    uintptr_t fl = spinlock_acquire_irqsave(&p->fd_table_lock);
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!p->fd_table[fd]) {
            p->fd_table[fd] = sf;
            spinlock_release_irqrestore(&p->fd_table_lock, fl);
            return fd;
        }
    }
    spinlock_release_irqrestore(&p->fd_table_lock, fl);
    return -EMFILE;
}

int sys_open(const char *path, int flags, int mode) {
    pcb_t *p = get_current_process();
    if (!p || !path) return -EFAULT;
    sys_file_t *sf = kmalloc(sizeof(*sf));
    if (!sf) return -ENOMEM;
    sf->dups = 0;
    sf->flags = flags;
    int rc = vfs_open(path, flags, (mode_t)mode, &sf->vfs_file);
    if (rc < 0) { kfree(sf); return rc; }
    rc = fd_install(p, sf);
    if (rc < 0) sys_file_put(sf);
    return rc;
}

int sys_close(int fd) {
    pcb_t *p = get_current_process();
    if (!p || fd < 0 || fd >= MAX_FD) return -EBADF;
    uintptr_t fl = spinlock_acquire_irqsave(&p->fd_table_lock);
    sys_file_t *sf = p->fd_table[fd];
    p->fd_table[fd] = NULL;
    spinlock_release_irqrestore(&p->fd_table_lock, fl);
    return sf ? sys_file_put(sf) : -EBADF;
}

/* Run `call` on the file behind fd, holding a reference across it. */
#define WITH_FILE(fd, call) \
    sys_file_t *sf = sys_file_get(fd); \
    if (!sf) return -EBADF; \
    file_t *f = sf->vfs_file; \
    (void)f; \
    __typeof__(call) rc = (call); \
    sys_file_put(sf); \
    return rc

ssize_t sys_read(int fd, void *buf, size_t count) { WITH_FILE(fd, vfs_read(f, buf, count)); }
ssize_t sys_write(int fd, const void *buf, size_t count) { WITH_FILE(fd, vfs_write(f, buf, count)); }
off_t sys_lseek(int fd, off_t off, int whence) { WITH_FILE(fd, vfs_lseek(f, off, whence)); }
int sys_getdents64(int fd, void *buf, size_t count) { WITH_FILE(fd, vfs_getdents64(f, buf, count)); }
int sys_fstat(int fd, struct kstat64 *st) { WITH_FILE(fd, vfs_fstat(f, st)); }

int sys_ioctl(int fd, uint32_t cmd, void *arg) { WITH_FILE(fd, vfs_ioctl(f, cmd, arg)); }

int sys_dup(int oldfd) {
    pcb_t *p = get_current_process();
    sys_file_t *sf = sys_file_get(oldfd);   /* this reference becomes the new slot's */
    if (!p || !sf) return -EBADF;
    int fd = fd_install(p, sf);
    if (fd < 0) sys_file_put(sf);
    return fd;
}

int sys_dup2(int oldfd, int newfd) {
    pcb_t *p = get_current_process();
    if (newfd < 0 || newfd >= MAX_FD) return -EBADF;
    sys_file_t *sf = sys_file_get(oldfd);
    if (!p || !sf) return -EBADF;
    if (oldfd == newfd) { sys_file_put(sf); return newfd; }
    uintptr_t fl = spinlock_acquire_irqsave(&p->fd_table_lock);
    sys_file_t *old = p->fd_table[newfd];
    p->fd_table[newfd] = sf;
    spinlock_release_irqrestore(&p->fd_table_lock, fl);
    if (old) sys_file_put(old);
    return newfd;
}

void sys_files_clone(pcb_t *dst, pcb_t *src) {
    uintptr_t fl = spinlock_acquire_irqsave(&src->fd_table_lock);
    for (int fd = 0; fd < MAX_FD; fd++) {
        sys_file_t *sf = src->fd_table[fd];
        dst->fd_table[fd] = sf ? sys_file_ref(sf) : NULL;
    }
    spinlock_release_irqrestore(&src->fd_table_lock, fl);
}
