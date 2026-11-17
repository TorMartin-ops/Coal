/*
 * Linux i386 ABI structures and constants used by the system call layer.
 * Layouts match what a static Linux/i386 binary expects.
 */
#ifndef KERNEL_CPU_LINUX_ABI_H
#define KERNEL_CPU_LINUX_ABI_H

#include <kernel/core/types.h>

/* errno values missing from fs_errno.h */
#ifndef EOVERFLOW
#define EOVERFLOW   75
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP  95
#endif

#define AT_FDCWD        (-100)
#define PATH_MAX_SYS    4096            /* Linux PATH_MAX */

/* open(2) flags beyond the access mode live in <kernel/fs/vfs/sys_file.h> */
#define LINUX_O_CLOEXEC 02000000

/* wait4 options */
#define WNOHANG         1

/* mmap */
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

/* ioctl */
#define TCGETS          0x5401
#define TIOCGWINSZ      0x5413

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct linux_timespec { int32_t tv_sec; int32_t tv_nsec; };
struct linux_timeval  { int32_t tv_sec; int32_t tv_usec; };

struct linux_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

/* struct stat64 of the i386 kernel ABI (96 bytes). */
struct linux_stat64 {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_sec, st_atime_nsec;
    uint32_t st_mtime_sec, st_mtime_nsec;
    uint32_t st_ctime_sec, st_ctime_nsec;
    uint64_t st_ino;
} __attribute__((packed));

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed));

struct linux_iovec { uint32_t iov_base; uint32_t iov_len; };

struct linux_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct linux_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

struct linux_user_desc {
    uint32_t entry_number, base_addr, limit;
    uint32_t flags;                     /* bit 4: limit_in_pages */
};

struct linux_sigaction {                /* i386 kernel struct sigaction */
    uint32_t handler, flags, restorer;
    uint32_t mask[2];
};

#endif
