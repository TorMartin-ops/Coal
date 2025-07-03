/**
 * @file stat.h
 * @brief File status structure and related definitions
 * @author Coal OS Kernel Team
 * @version 1.0
 */

#ifndef STAT_H
#define STAT_H

#include <libc/stdint.h>
#include <kernel/core/types.h>

// File type and mode constants (POSIX compatible)
#define S_IFMT   0170000  // File type mask
#define S_IFREG  0100000  // Regular file
#define S_IFDIR  0040000  // Directory
#define S_IFCHR  0020000  // Character device
#define S_IFBLK  0060000  // Block device
#define S_IFIFO  0010000  // FIFO (named pipe)
#define S_IFLNK  0120000  // Symbolic link
#define S_IFSOCK 0140000  // Socket

// File mode bits
#define S_ISUID  0004000  // Set UID on execution
#define S_ISGID  0002000  // Set GID on execution
#define S_ISVTX  0001000  // Sticky bit

// Permission bits
#define S_IRUSR  0000400  // Owner read permission
#define S_IWUSR  0000200  // Owner write permission
#define S_IXUSR  0000100  // Owner execute permission
#define S_IRGRP  0000040  // Group read permission
#define S_IWGRP  0000020  // Group write permission
#define S_IXGRP  0000010  // Group execute permission
#define S_IROTH  0000004  // Others read permission
#define S_IWOTH  0000002  // Others write permission
#define S_IXOTH  0000001  // Others execute permission

// Macros to test file type
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)  // Is regular file
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)  // Is directory
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)  // Is character device
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)  // Is block device
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)  // Is FIFO
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)  // Is symbolic link
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK) // Is socket

// Time type for file timestamps
#ifndef _TIME_T_DEFINED
typedef uint32_t time_t;
#define _TIME_T_DEFINED
#endif

// Number of links type
#ifndef _NLINK_T_DEFINED
typedef uint32_t nlink_t;
#define _NLINK_T_DEFINED
#endif

// Block size type
typedef uint32_t blksize_t;

// Block count type
typedef uint32_t blkcnt_t;

/**
 * @brief File status structure (POSIX compatible)
 */
struct stat {
    dev_t     st_dev;     // Device ID of device containing file
    ino_t     st_ino;     // File serial number (inode number)
    mode_t    st_mode;    // File mode (type and permissions)
    nlink_t   st_nlink;   // Number of hard links
    uid_t     st_uid;     // User ID of file owner
    gid_t     st_gid;     // Group ID of file owner
    dev_t     st_rdev;    // Device ID (if file is character or block special)
    off_t     st_size;    // File size in bytes (for regular files)
    time_t    st_atime;   // Time of last access
    time_t    st_mtime;   // Time of last modification
    time_t    st_ctime;   // Time of last status change
    blksize_t st_blksize; // Preferred block size for I/O
    blkcnt_t  st_blocks;  // Number of blocks allocated
};

#endif // STAT_H