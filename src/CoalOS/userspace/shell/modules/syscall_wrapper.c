/**
 * @file syscall_wrapper.c
 * @brief System call wrapper implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "syscall_wrapper.h"

//============================================================================
// System Call Implementation
//============================================================================

int32_t syscall(int32_t syscall_number, int32_t arg1, int32_t arg2, int32_t arg3) {
    int32_t return_value;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "movl %1, %%eax\n\t"
        "movl %2, %%ebx\n\t"
        "movl %3, %%ecx\n\t"
        "movl %4, %%edx\n\t"
        "int $0x80\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%ebx\n\t"
        : "=a" (return_value)
        : "m" (syscall_number), "m" (arg1), "m" (arg2), "m" (arg3)
        : "cc", "memory"
    );
    return return_value;
}