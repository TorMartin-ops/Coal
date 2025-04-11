#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

// System call numbers.
enum {
    SYS_WRITE  = 1,
    SYS_EXIT   = 2,
    SYS_MMAP   = 3,
    SYS_MUNMAP = 4,
    SYS_BRK    = 5, // Added brk syscall number
    // Add more syscalls up to MAX_SYSCALL
};

// --- mmap protection flags ---
#define PROT_NONE       0x0     /* Page cannot be accessed */
#define PROT_READ       0x1     /* Page can be read */
#define PROT_WRITE      0x2     /* Page can be written */
#define PROT_EXEC       0x4     /* Page can be executed */

// --- mmap mapping flags ---
#define MAP_SHARED      0x01    /* Share changes */
#define MAP_PRIVATE     0x02    /* Changes are private (copy-on-write) */
#define MAP_FIXED       0x10    /* Interpret addr exactly */
#define MAP_ANONYMOUS   0x20    /* Don't use a file */
// Add other flags like MAP_NORESERVE, MAP_POPULATE as needed


typedef struct syscall_context {
    // Pushed by syscall_handler_asm (order: pusha, segments)
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;

    // Pushed by CPU during interrupt
    uint32_t eip, cs, eflags, user_esp, user_ss;
} syscall_context_t;


int syscall_handler(syscall_context_t *ctx);

#endif // SYSCALL_H