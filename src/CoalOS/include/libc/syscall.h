/* System call convention: number in eax, arguments in ebx, ecx, edx, esi, edi, ebp, result or -errno in eax.
 * Calls go through __vsyscall: the kernel's __kernel_vsyscall (sysenter) when the program was started with
 * AT_SYSINFO, else a stub in libc that executes int 0x80. Both preserve every register but eax. */
#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

#include <libc/syscall_numbers.h>

/* Linux numbers the shared table does not list (it only names what the kernel serves). */
#ifndef SYS_MOUNT
#define SYS_MOUNT   21
#endif
#ifndef SYS_UTIME
#define SYS_UTIME   30
#endif
#ifndef SYS_SYNC
#define SYS_SYNC    36
#endif
#ifndef SYS_RENAME
#define SYS_RENAME  38
#endif
#ifndef SYS_UMOUNT2
#define SYS_UMOUNT2 52
#endif
#ifndef SYS_REBOOT
#define SYS_REBOOT  88
#endif
#ifndef SYS_SYSLOG
#define SYS_SYSLOG  103
#endif
#ifndef SYS_FCNTL64
#define SYS_FCNTL64 221
#endif

/* Linux numbers the kernel may not serve (the wrappers then fail with ENOSYS). */
#ifndef SYS_LINK
#define SYS_LINK       9
#endif
#ifndef SYS_CHMOD
#define SYS_CHMOD      15
#endif
#ifndef SYS_SYMLINK
#define SYS_SYMLINK    83
#endif
#ifndef SYS_READLINK
#define SYS_READLINK   85
#endif
#ifndef SYS_STATFS
#define SYS_STATFS     99
#endif
#ifndef SYS_NEWSELECT
#define SYS_NEWSELECT  142
#endif
#ifndef SYS_SOCKETPAIR
#define SYS_SOCKETPAIR 360
#endif

extern void (*__vsyscall)(void);

/* Returns the raw kernel result; -4095..-1 is -errno. */
static inline long __sc(long n, long a, long b, long c, long d, long e) {
    long r;
    __asm__ volatile("call *%[entry]"
                     : "=a"(r)
                     : [entry] "m"(__vsyscall), "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory", "cc");
    return r;
}

#define __SC3(n, a, b, c) __sc((n), (long)(a), (long)(b), (long)(c), 0, 0)
#define __SC1(n, a)       __SC3(n, a, 0, 0)

/* Converts a raw result to the C convention: -1 and errno on error. */
long __syscall_ret(long r);

#endif
