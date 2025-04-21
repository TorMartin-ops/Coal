#ifndef SYSCALL_H
#define SYSCALL_H

#include <libc/stdint.h> // For uint32_t etc.

// === SYSTEM CALL NUMBERS ===
// These numbers should match what user programs expect

// User program syscall numbers (hello.c uses these)
#define USER_SYS_WRITE 1
#define USER_SYS_EXIT  2

// Internal kernel syscall numbers (for implementation)
// These are kept for backwards compatibility with existing code
#define SYS_EXIT    1
#define SYS_FORK    2   // Example
#define SYS_READ    3   // Example
#define SYS_WRITE   4
// Add other syscall numbers...
#define SYS_OPEN    5   // Example
#define SYS_CLOSE   6   // Example
// ... up to your maximum number

/**
 * @brief Structure holding the register context saved by the syscall handler stub.
 * The order MUST exactly match the order of registers pushed by the
 * syscall assembly handler (`syscall_handler_asm` in syscall.asm).
 * Typically corresponds to `pusha` order plus segments and original stack ptrs.
 */
typedef struct syscall_context {
    // Registers saved by pusha (adjust order/content based on syscall.asm!)
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy; // ESP value before pusha, often ignored
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;       // Syscall number input, return value output

    // Segment registers saved manually (if needed)
    uint32_t ds;
    uint32_t es;
    uint32_t fs;
    uint32_t gs;

    // Information pushed by CPU during interrupt/trap (if syscall uses INT)
    // uint32_t eip;    // User EIP
    // uint32_t cs;     // User CS
    // uint32_t eflags; // User EFLAGS
    // uint32_t esp_user; // User ESP (if privilege change)
    // uint32_t ss_user;  // User SS (if privilege change)

} __attribute__((packed)) syscall_context_t; // Use packed if needed

/**
 * @brief The main C entry point for system calls.
 * Called by the assembly stub (`syscall_handler_asm`).
 *
 * @param ctx Pointer to the saved register context.
 * @return Value to place back into the user process's EAX register.
 */
int syscall_handler(syscall_context_t *ctx);

#endif // SYSCALL_H