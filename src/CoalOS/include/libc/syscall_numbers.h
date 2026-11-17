/*
 * System call numbers as seen by user programs.
 *
 * These must match include/kernel/cpu/syscall.h. The script
 * scripts/check_syscall_numbers.sh compares the two files; run it after
 * changing either of them.
 */
#ifndef LIBC_SYSCALL_NUMBERS_H
#define LIBC_SYSCALL_NUMBERS_H

/* Linux i386 compatible numbers */
#define SYS_EXIT     1
#define SYS_FORK     2
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7
#define SYS_UNLINK   10
#define SYS_EXECVE   11
#define SYS_CHDIR    12
#define SYS_LSEEK    19
#define SYS_GETPID   20
#define SYS_KILL     37
#define SYS_MKDIR    39
#define SYS_RMDIR    40
#define SYS_PIPE     42
#define SYS_BRK      45
#define SYS_SIGNAL   48
#define SYS_SETPGID  57
#define SYS_DUP2     63
#define SYS_GETPPID  64
#define SYS_GETPGRP  65
#define SYS_SETSID   66
#define SYS_MMAP     90
#define SYS_STAT     106
#define SYS_GETPGID  132
#define SYS_GETDENTS 141
#define SYS_GETSID   147
#define SYS_GETCWD   183
#define SYS_TCSETPGRP 410
#define SYS_TCGETPGRP 411

/* Coal OS specific */
#define SYS_PUTS               7000
#define SYS_READ_TERMINAL_LINE 7001

#endif /* LIBC_SYSCALL_NUMBERS_H */
