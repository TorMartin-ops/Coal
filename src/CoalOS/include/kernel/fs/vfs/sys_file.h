/**
 * @file sys_file.h
 * @brief Per-process file descriptor layer on top of the VFS.
 *
 * Buffers are kernel pointers (the syscall layer copies from/to user space).
 * Return values are >= 0 or a negative Linux errno.
 */
#ifndef SYS_FILE_H
#define SYS_FILE_H

#include <kernel/core/types.h>
#include <kernel/fs/vfs/vfs.h>

struct pcb;

/**
 * An open file description shared by every descriptor that duplicates it
 * (dup, fork). `dups` counts the references beyond the first, so a freshly
 * zeroed structure holds exactly one reference.
 */
typedef struct sys_file {
    file_t *vfs_file;
    int     flags;
    int     dups;
} sys_file_t;

int     sys_open(const char *path, int flags, int mode);
int     sys_close(int fd);
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_write(int fd, const void *buf, size_t count);
off_t   sys_lseek(int fd, off_t offset, int whence);
int     sys_getdents64(int fd, void *buf, size_t count);
int     sys_fstat(int fd, struct kstat64 *st);
int     sys_ioctl(int fd, uint32_t cmd, void *arg);
int     sys_dup(int oldfd);
int     sys_dup2(int oldfd, int newfd);
/** Anonymous pipe: fds[0] reads what is written to fds[1]. flags: O_NONBLOCK. */
int     sys_pipe(int fds[2], int flags);

/** Open file on a driver-private vnode (sockets): new descriptor in the current process, or -errno.
 *  The driver's close() runs when the last descriptor goes; on error it still owns data. */
int     sys_install_vnode(vfs_driver_t *drv, void *data, uint32_t type, int flags);

/** Look up fd in the current process and take a reference; NULL if invalid. */
sys_file_t *sys_file_get(int fd);
/** Take an extra reference (fork, dup). */
sys_file_t *sys_file_ref(sys_file_t *sf);
/** Drop a reference; the last one closes the file. Returns the close status. */
int sys_file_put(sys_file_t *sf);
/** Share every open file of `src` with `dst` (fork). */
void sys_files_clone(struct pcb *dst, struct pcb *src);

#endif /* SYS_FILE_H */
