/*
 * The one system call entry point for user programs.
 *
 * The kernel takes the number in EAX and up to three arguments in EBX, ECX
 * and EDX (int 0x80) and returns the result in EAX. Negative results are
 * negated error numbers.
 */
#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

#include <libc/syscall_numbers.h>

static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int result;
    __asm__ volatile ("int $0x80"
                      : "=a"(result)
                      : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
                      : "memory", "cc");
    return result;
}

#endif /* LIBC_SYSCALL_H */
