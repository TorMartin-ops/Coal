#ifndef SYSCALL_H
#define SYSCALL_H

#include <libc/stdint.h> // For uint32_t etc.
#include <libc/stddef.h> // For size_t

// Define system call numbers (POSIX-like, adjust as needed)
#define SYS_EXIT     1
#define SYS_FORK     2   // Not Implemented
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7   // Not Implemented
// #define SYS_CREAT    8 // Deprecated by open
// #define SYS_LINK     9 // Not Implemented
// #define SYS_UNLINK  10 // Not Implemented
// #define SYS_EXECVE  11 // Not Implemented
// #define SYS_CHDIR   12 // Not Implemented
#define SYS_LSEEK    19  // Matches Linux convention
// Add other syscalls as needed

/**
 * @brief Structure holding the register context saved by the syscall handler stub.
 * The order MUST exactly match the order of registers pushed by the
 * syscall assembly handler (`syscall_handler_asm` in syscall.asm).
 * Matches pusha order + segments.
 */
typedef struct syscall_context {
    // Registers saved by pusha (in order: EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX)
    uint32_t edi;       // 5th syscall argument (or saved register)
    uint32_t esi;       // 4th syscall argument (or saved register)
    uint32_t ebp;       // 6th syscall argument (or saved register / frame pointer)
    uint32_t esp_dummy; // ESP value before pusha, often ignored
    uint32_t ebx;       // 1st syscall argument
    uint32_t edx;       // 3rd syscall argument
    uint32_t ecx;       // 2nd syscall argument
    uint32_t eax;       // Syscall number input, return value output

    // Segment registers saved manually (gs, fs, es, ds)
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;

    // Information pushed by CPU during interrupt/trap (INT 0x80) is handled by iret
    // No need to explicitly store user EIP, CS, EFLAGS, ESP, SS here as iret uses
    // the values already on the stack placed there by the CPU.

} __attribute__((packed)) syscall_context_t; // Use packed if needed

/**
 * @brief The main C entry point for system calls.
 * Called by the assembly stub (`syscall_handler_asm`).
 *
 * @param ctx Pointer to the saved register context.
 * @return Value to place back into the user process's EAX register (typically 0 on success, negative errno on error).
 */
int syscall_handler(syscall_context_t *ctx);

#endif // SYSCALL_H
