#ifndef SYSCALL_H
#define SYSCALL_H

#include <libc/stdint.h> // For uint32_t etc.
#include <libc/stddef.h> // For size_t
#include "types.h"       // For off_t, ssize_t

// Define system call numbers (Add more as needed)
#define SYS_EXIT     1
#define SYS_FORK     2  // Example: Not Implemented
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7  // Example: Not Implemented
#define SYS_LSEEK    19 // Matches Linux convention
// Define other syscall numbers...
#define SYS_GETPID  20 // Example: Not Implemented
#define SYS_BRK     45 // Example: Not Implemented
#define SYS_MMAP    90 // Example: Not Implemented
#define SYS_MUNMAP  91 // Example: Not Implemented

// Define the maximum number of system calls
#define MAX_SYSCALLS 256 // Adjust as needed

/**
 * @brief Represents the register state passed from the assembly stub.
 * The order MUST match the pushes in syscall_handler_asm AFTER EAX
 * (which holds the syscall number) and BEFORE the manual segment pushes.
 * This structure might be less critical if syscall implementation functions
 * directly take arguments passed via registers/stack by the modified asm stub.
 * However, it's useful for accessing the original register state if needed,
 * especially EAX for the return value.
 */
typedef struct syscall_regs {
    // Registers saved manually *before* calling C handler (order matters!)
    // Corresponds to pushes in syscall.asm
    uint32_t ebp;
    uint32_t edi;
    uint32_t esi;
    uint32_t edx;
    uint32_t ecx;
    uint32_t ebx;

    // Segment registers saved manually
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;

    // Original EAX pushed by the stub (contains syscall number on entry)
    // This will be overwritten with the return value.
    uint32_t eax;

    // CPU-pushed state (iret will use these from the stack)
    // uint32_t eip;
    // uint32_t cs;
    // uint32_t eflags;
    // uint32_t user_esp; // If CPL change
    // uint32_t user_ss;  // If CPL change

} __attribute__((packed)) syscall_regs_t;

/**
 * @brief Function pointer type for system call implementations.
 * Arguments correspond to typical syscall arguments (e.g., Linux convention):
 * arg0 (EBX), arg1 (ECX), arg2 (EDX), arg3 (ESI), arg4 (EDI), arg5 (EBP)
 * Return value is placed in EAX by the handler.
 */
typedef int (*syscall_fn_t)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);


/**
 * @brief Initializes the system call dispatcher table.
 */
void syscall_init(void);

/**
 * @brief The main C entry point for system calls.
 * Called by the assembly stub (`syscall_handler_asm`).
 *
 * @param regs Pointer to the saved register context on the kernel stack.
 * Note: EAX within this structure initially holds the syscall number.
 * The handler must place the return value back into regs->eax.
 */
void syscall_dispatcher(syscall_regs_t *regs);

#endif // SYSCALL_H