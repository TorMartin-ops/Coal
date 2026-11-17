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
#include <kernel/sync/spinlock.h>

struct pcb;

/**
 * An open file description shared by every descriptor that duplicates it
 * (dup, fork). `dups` counts the references beyond the first, so a freshly
 * zeroed structure holds exactly one reference.
 */
typedef struct sys_file {
    file_t *vfs_file;
<<<<<<< HEAD
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
int     sys_stat(const char *path, struct kstat64 *st);
int     sys_ftruncate(int fd, off_t len);
int     sys_fsync(int fd);
int     sys_ioctl(int fd, uint32_t cmd, void *arg);
int     sys_dup(int oldfd);
int     sys_dup2(int oldfd, int newfd);
=======
    int flags;
    int extra_refs;      /* references beyond the first (fork/dup); zero-initialised = one owner */
} sys_file_t;

/** Adds a reference (fork, dup). */
static inline sys_file_t *sys_file_get(sys_file_t *sf) {
    uintptr_t flags = local_irq_save();      /* i386 has no atomic add; one CPU */
    sf->extra_refs++;
    local_irq_restore(flags);
    return sf;
}

/** Drops a reference; true when it was the last one and the caller must close and free sf. */
static inline bool sys_file_put(sys_file_t *sf) {
    uintptr_t flags = local_irq_save();
    bool last = sf->extra_refs-- == 0;
    local_irq_restore(flags);
    return last;
}

>>>>>>> wave2-process

/** Look up fd in the current process and take a reference; NULL if invalid. */
sys_file_t *sys_file_get(int fd);
/** Take an extra reference (fork, dup). */
sys_file_t *sys_file_ref(sys_file_t *sf);
/** Drop a reference; the last one closes the file. Returns the close status. */
int sys_file_put(sys_file_t *sf);
/** Share every open file of `src` with `dst` (fork). */
void sys_files_clone(struct pcb *dst, struct pcb *src);

#endif /* SYS_FILE_H */
