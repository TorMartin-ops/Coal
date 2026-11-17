/*
 * System call numbers: the Linux i386 ABI (int 0x80, number in EAX,
 * arguments in EBX, ECX, EDX, ESI, EDI, EBP, result or -errno in EAX).
 *
 * The kernel includes this header too, so there is one copy. Calls that
 * Coal does not implement fail with -ENOSYS. scripts/check_syscall_numbers.sh
 * verifies every number against the Linux headers.
 */
#ifndef LIBC_SYSCALL_NUMBERS_H
#define LIBC_SYSCALL_NUMBERS_H

#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_READ            3
#define SYS_WRITE           4
#define SYS_OPEN            5
#define SYS_CLOSE           6
#define SYS_WAITPID         7
#define SYS_UNLINK          10
#define SYS_EXECVE          11
#define SYS_CHDIR           12
#define SYS_TIME            13
#define SYS_LSEEK           19
#define SYS_GETPID          20
#define SYS_GETUID          24
#define SYS_ACCESS          33
#define SYS_KILL            37
#define SYS_MKDIR           39
#define SYS_RMDIR           40
#define SYS_DUP             41
#define SYS_PIPE            42
#define SYS_BRK             45
#define SYS_GETGID          47
#define SYS_GETEUID         49
#define SYS_GETEGID         50
#define SYS_IOCTL           54
#define SYS_SETPGID         57
#define SYS_DUP2            63
#define SYS_GETPPID         64
#define SYS_GETPGRP         65
#define SYS_SETSID          66
#define SYS_GETTIMEOFDAY    78
#define SYS_MUNMAP          91
#define SYS_WAIT4           114
#define SYS_UNAME           122
#define SYS_GETPGID         132
#define SYS_LLSEEK          140
#define SYS_READV           145
#define SYS_WRITEV          146
#define SYS_GETSID          147
#define SYS_SCHED_YIELD     158
#define SYS_NANOSLEEP       162
#define SYS_RT_SIGACTION    174
#define SYS_RT_SIGPROCMASK  175
#define SYS_GETCWD          183
#define SYS_MMAP2           192
#define SYS_STAT64          195
#define SYS_LSTAT64         196
#define SYS_FSTAT64         197
#define SYS_GETUID32        199
#define SYS_GETGID32        200
#define SYS_GETEUID32       201
#define SYS_GETEGID32       202
#define SYS_GETDENTS64      220
#define SYS_GETTID          224
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP      252
#define SYS_SET_TID_ADDRESS 258
#define SYS_CLOCK_GETTIME   265
#define SYS_OPENAT          295
#define SYS_FSTATAT64       300
#define SYS_PIPE2           331

/* Coal-only calls live far above the Linux range. */
#define SYS_READ_TERMINAL_LINE 0x1001

#endif /* LIBC_SYSCALL_NUMBERS_H */
