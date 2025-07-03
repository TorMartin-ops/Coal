/**
 * @file stat.h
 * @brief File status structure definitions
 * @author Coal OS Development Team
 * @version 1.0
 */

#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <libc/stdint.h>
#include <kernel/core/types.h>

/* Define missing types if not already defined */
#ifndef _NLINK_T_DEFINED
#define _NLINK_T_DEFINED
typedef uint32_t nlink_t;
#endif

#ifndef _BLKSIZE_T_DEFINED
#define _BLKSIZE_T_DEFINED
typedef uint32_t blksize_t;
#endif

#ifndef _BLKCNT_T_DEFINED
#define _BLKCNT_T_DEFINED
typedef uint64_t blkcnt_t;
#endif

#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef uint64_t time_t;
#endif

/* File types for st_mode */
#define S_IFMT      0170000     /* type of file mask */
#define S_IFREG     0100000     /* regular file */
#define S_IFDIR     0040000     /* directory */
#define S_IFCHR     0020000     /* character special */
#define S_IFBLK     0060000     /* block special */

/* File permissions */
#define S_IRUSR     0400        /* owner has read permission */
#define S_IWUSR     0200        /* owner has write permission */
#define S_IXUSR     0100        /* owner has execute permission */
#define S_IRGRP     0040        /* group has read permission */
#define S_IWGRP     0020        /* group has write permission */
#define S_IXGRP     0010        /* group has execute permission */
#define S_IROTH     0004        /* others have read permission */
#define S_IWOTH     0002        /* others have write permission */
#define S_IXOTH     0001        /* others have execute permission */

/* Macros to test file type */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)

/**
 * @brief File status structure
 */
struct stat {
    dev_t     st_dev;     /* ID of device containing file */
    ino_t     st_ino;     /* inode number */
    mode_t    st_mode;    /* file type and permissions */
    nlink_t   st_nlink;   /* number of hard links */
    uid_t     st_uid;     /* user ID of owner */
    gid_t     st_gid;     /* group ID of owner */
    dev_t     st_rdev;    /* device ID (if special file) */
    off_t     st_size;    /* total size in bytes */
    blksize_t st_blksize; /* blocksize for filesystem I/O */
    blkcnt_t  st_blocks;  /* number of 512B blocks allocated */
    time_t    st_atime;   /* time of last access */
    time_t    st_mtime;   /* time of last modification */
    time_t    st_ctime;   /* time of last status change */
};

#endif /* SYS_STAT_H */