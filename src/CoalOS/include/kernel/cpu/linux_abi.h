/*
 * Linux i386 ABI structures and constants used by the system call layer.
 * Layouts match what a static Linux/i386 binary expects.
 */
#ifndef KERNEL_CPU_LINUX_ABI_H
#define KERNEL_CPU_LINUX_ABI_H

#include <kernel/core/types.h>

/* errno values missing from fs_errno.h */
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#ifndef EOVERFLOW
#define EOVERFLOW   75
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP  95
#endif

#define AT_FDCWD        (-100)
#define PATH_MAX_SYS    4096            /* Linux PATH_MAX; same limit as the VFS */

/* open(2) flags beyond the access mode live in <kernel/fs/vfs/sys_file.h> */
#define LINUX_O_CLOEXEC 02000000

/* mmap */
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct linux_timespec { int32_t tv_sec; int32_t tv_nsec; };
struct linux_timeval  { int32_t tv_sec; int32_t tv_usec; };

struct linux_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct linux_iovec { uint32_t iov_base; uint32_t iov_len; };

struct linux_user_desc {
    uint32_t entry_number, base_addr, limit;
    uint32_t flags;                     /* bit 4: limit_in_pages */
};

struct linux_sigaction {                /* i386 kernel struct sigaction */
    uint32_t handler, flags, restorer;
    uint32_t mask[2];
};

#define SA_NOCLDSTOP    0x00000001
#define SA_SIGINFO      0x00000004
#define SA_RESTART      0x10000000
#define SA_RESTORER     0x04000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000u
#define SIG_DFL         0
#define SIG_IGN         1
#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

/* fcntl */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define FD_CLOEXEC      1

/* poll */
struct linux_pollfd { int32_t fd; int16_t events, revents; };

/* reboot */
#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_CMD_RESTART    0x01234567
#define LINUX_REBOOT_CMD_HALT       0xCDEF0123
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321FEDC

/* syslog(2) actions */
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_SIZE_BUFFER   10

#endif
