/**
 * @file vfs.h
 * @brief Virtual file system: driver interface, mount table and file API.
 *
 * All functions return 0/positive on success and a negative Linux errno on
 * failure. Paths are absolute; the VFS normalises them (".", "..", "//").
 */
#ifndef VFS_H
#define VFS_H

#include <kernel/core/types.h>
#include <kernel/fs/vfs/fs_errno.h>

#define VFS_PATH_MAX   4096     /* longest accepted absolute path, incl. NUL (Linux PATH_MAX) */
#define VFS_NAME_MAX   255

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* open(2) flags, Linux i386 values */
#define O_ACCMODE   0x0003
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000

#ifndef S_IFMT
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#endif

/* poll(2) event bits, Linux values */
#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8

/** Layout of Linux i386 struct stat64 (what fstat64/stat64 copy to user). */
struct kstat64 {
    uint64_t st_dev;
    uint32_t __pad0;
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t __pad3;
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime, st_atime_nsec;
    uint32_t st_mtime, st_mtime_nsec;
    uint32_t st_ctime, st_ctime_nsec;
    uint64_t st_ino;
} __attribute__((packed));

/** Layout of Linux struct statfs64 on i386 (84 bytes). */
struct kstatfs64 {
    uint32_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} __attribute__((packed));

/** Layout of Linux struct linux_dirent64 (getdents64 records). */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed));

/** One directory entry as produced by a driver's readdir. */
struct vfs_dirent {
    uint32_t ino;
    uint8_t  type;                      /* DT_* */
    char     name[VFS_NAME_MAX + 1];
};

typedef struct vfs_driver vfs_driver_t;
typedef struct mount mount_t;

/** An open object inside a file system (shared by all file_t that open it). */
typedef struct vnode {
    void         *data;                 /* driver private */
    vfs_driver_t *fs_driver;
    mount_t      *mnt;
    uint32_t      type;                 /* S_IFDIR / S_IFREG / S_IFCHR, set by driver open */
} vnode_t;

/** An open file description: offset and flags on top of a vnode. */
typedef struct file {
    vnode_t *vnode;
    uint32_t flags;                     /* open flags */
    off_t    offset;
} file_t;

/**
 * Driver operations. Only open/close are mandatory. Data operations take an
 * explicit position so a driver keeps no per-descriptor state; the VFS owns
 * file->offset. Paths handed to a driver are relative to its mount point and
 * start with '/'.
 */
struct vfs_driver {
    const char *fs_name;
    uint32_t magic;                                 /* statfs f_type */
    /** Space figures of the file system; without it statfs reports 0 blocks. */
    int   (*statfs)(void *fs, struct kstatfs64 *st);
    void *(*mount)(const char *device);
    int   (*unmount)(void *fs);                     /* -EBUSY if still in use */
    /** Open or create. Must honour O_CREAT/O_EXCL; fills v->data and v->type. */
    int   (*open)(void *fs, const char *path, int flags, mode_t mode, vnode_t *v);
    int   (*close)(vnode_t *v);
    ssize_t (*read)(vnode_t *v, off_t pos, void *buf, size_t len);
    ssize_t (*write)(vnode_t *v, off_t pos, const void *buf, size_t len);
    /** Returns 1 and advances *pos for each entry, 0 at the end. */
    int   (*readdir)(vnode_t *dir, off_t *pos, struct vfs_dirent *out);
    int   (*getattr)(vnode_t *v, struct kstat64 *st);
    int   (*truncate)(vnode_t *v, off_t len);
    int   (*fsync)(vnode_t *v);
    int   (*sync)(void *fs);                        /* flush the whole file system */
    int   (*ioctl)(vnode_t *v, uint32_t cmd, void *arg);
    int   (*utime)(vnode_t *v, uint32_t atime, uint32_t mtime);
    /** Readiness: POLLIN/POLLOUT/POLLHUP bits. Must not block. Without it a file is always ready. */
    void  (*set_nonblock)(vnode_t *v, bool on);     /* fcntl(F_SETFL) changed O_NONBLOCK */
    uint32_t (*poll)(vnode_t *v);
    int   (*unlink)(void *fs, const char *path);
    int   (*mkdir)(void *fs, const char *path, mode_t mode);
    int   (*rmdir)(void *fs, const char *path);
    int   (*rename)(void *fs, const char *from, const char *to);
};

/* Registration and mounting */
void vfs_init(void);
int  vfs_register_driver(vfs_driver_t *drv);
int  vfs_mount(const char *target, const char *fs_name, const char *device);
int  vfs_umount(const char *target);
void vfs_sync(void);
void vfs_shutdown(void);

/* Paths */
int  vfs_normalize(const char *path, char *out, size_t size);

/* Files */
int     vfs_open(const char *path, int flags, mode_t mode, file_t **out);
int     vfs_close(file_t *f);
ssize_t vfs_read(file_t *f, void *buf, size_t len);
ssize_t vfs_write(file_t *f, const void *buf, size_t len);
off_t   vfs_lseek(file_t *f, off_t off, int whence);
int     vfs_getdents64(file_t *f, void *buf, size_t len);
int     vfs_fstat(file_t *f, struct kstat64 *st);
int     vfs_fstatfs(file_t *f, struct kstatfs64 *st);
int     vfs_ftruncate(file_t *f, off_t len);
int     vfs_fsync(file_t *f);
int     vfs_ioctl(file_t *f, uint32_t cmd, void *arg);
uint32_t vfs_poll(file_t *f);

/* Namespace operations */
int  vfs_stat(const char *path, struct kstat64 *st);
int  vfs_statfs(const char *path, struct kstatfs64 *st);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path, mode_t mode);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *from, const char *to);
int  vfs_utime(const char *path, uint32_t atime, uint32_t mtime);

/** Seconds since the Unix epoch used for new timestamps. */
uint32_t vfs_time(void);

#endif /* VFS_H */
