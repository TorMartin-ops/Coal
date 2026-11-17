/* System call convention (Linux x86_64): number in rax, arguments in rdi, rsi, rdx, r10, r8, r9, result or
 * -errno in rax. The SYSCALL instruction clobbers rcx and r11. */
#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

#include <libc/syscall_numbers.h>

/* A Linux number the kernel may not serve: the wrapper then fails with ENOSYS. */
#ifndef SYS_CHMOD
#define SYS_CHMOD      90
#endif

/* Returns the raw kernel result; -4095..-1 is -errno. */
static inline long __sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return r;
}

static inline long __sc(long n, long a, long b, long c, long d, long e) { return __sc6(n, a, b, c, d, e, 0); }

#define __SC3(n, a, b, c) __sc((n), (long)(a), (long)(b), (long)(c), 0, 0)
#define __SC1(n, a)       __SC3(n, a, 0, 0)

/* Converts a raw result to the C convention: -1 and errno on error. */
long __syscall_ret(long r);

#endif
