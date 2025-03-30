#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include "libc/stddef.h"   // For size_t, NULL
#include "libc/stdint.h"   // For uint32_t, etc.
#include "libc/stdbool.h"  // For bool, true, false

// System call numbers.
enum {
    SYS_WRITE = 1,
    SYS_EXIT  = 2,
    // Additional syscalls can be defined here.
};

/**
 * Structure representing the register context for a system call.
 * The syscall number is in EAX; arguments are passed in EBX, ECX, EDX, etc.
 */
typedef struct syscall_context {
    uint32_t eax;   // Syscall number (and return value)
    uint32_t ebx;   // Argument 1
    uint32_t ecx;   // Argument 2
    uint32_t edx;   // Argument 3
    uint32_t esi;   // Argument 4
    uint32_t edi;   // Argument 5
    uint32_t ebp;   // Base pointer
} syscall_context_t;

/**
 * Handles a system call.
 * This function is called from the syscall assembly stub.
 *
 * @param ctx Pointer to the syscall context.
 * @return The value to be returned in EAX.
 */
int syscall_handler(syscall_context_t *ctx) __attribute__((used));

#endif // SYSCALL_H
