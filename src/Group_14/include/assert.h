// assert.h - Kernel Assertion Handler

#ifndef ASSERT_H
#define ASSERT_H

#include "terminal.h" // For terminal_printf and KERNEL_PANIC_HALT (or similar)

// Ensure KERNEL_PANIC_HALT is defined (can be defined here or in terminal.h)
#ifndef KERNEL_PANIC_HALT
#define KERNEL_PANIC_HALT(msg) do { \
    terminal_printf("\n[KERNEL PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
    while(1) { asm volatile("cli; hlt"); } \
} while(0)
#endif

/**
 * @brief Kernel Assertion Check
 * If the condition `expr` evaluates to false (0), prints an error message
 * including the expression, file name, and line number, then halts the system.
 *
 * @param expr The expression to evaluate.
 * @param msg A descriptive message string explaining the assertion.
 */
#define KERNEL_ASSERT(expr, msg) do { \
    if (!(expr)) { \
        terminal_printf("\n[ASSERT FAILED] %s\n", msg); \
        terminal_printf(" Expression: %s\n", #expr); \
        terminal_printf(" File: %s, Line: %d\n", __FILE__, __LINE__); \
        KERNEL_PANIC_HALT("Assertion failed"); \
    } \
} while (0)


// Optional: Define a standard `assert` macro if needed for compatibility,
// perhaps only enabled in debug builds.
#ifdef DEBUG
 #define assert(expr) KERNEL_ASSERT(expr, "Assertion failed")
#else
 #define assert(expr) ((void)0) // Disable standard assert in release builds
#endif


#ifndef KERNEL_PANIC_HALT // Avoid redefinition if defined elsewhere
#define KERNEL_PANIC_HALT(msg) do { \
    asm volatile ("cli"); /* Disable interrupts */ \
    terminal_printf("\n[KERNEL PANIC] %s\n", msg); \
    terminal_printf("  at %s:%d\n", __FILE__, __LINE__); \
    terminal_printf("System Halted.\n"); \
    while(1) { asm volatile ("hlt"); } /* Halt the CPU */ \
} while(0)
#endif // KERNEL_PANIC_HALT


#endif // ASSERT_H