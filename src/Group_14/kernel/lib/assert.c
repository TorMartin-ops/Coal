// src/assert.c
// Provides implementation for assertion handling, including the panic wrapper for ISRs.

#include <kernel/lib/assert.h>   // Include the header defining KERNEL_ASSERT and KERNEL_PANIC_HALT
#include <kernel/drivers/display/terminal.h> // Include for terminal_printf used by the panic macro

/**
 * @brief C wrapper function to invoke kernel panic from assembly ISRs.
 * This provides a C linkage point for assembly code (like isr_pf.asm)
 * to trigger the panic macro defined in assert.h.
 * It MUST NOT be declared 'static' so the linker can find it.
 */
void invoke_kernel_panic_from_isr(void) {
    // This function is called directly from assembly when an unhandled
    // kernel fault (that isn't covered by the exception table) occurs.
    // It uses the KERNEL_PANIC_HALT macro from assert.h to handle the actual panic.
    KERNEL_PANIC_HALT("Unhandled KERNEL Fault from ISR!");

    // This part should technically be unreachable as KERNEL_PANIC_HALT halts.
    // Add cli/hlt just as a safeguard in case the macro definition changes.
    asm volatile ("cli; hlt");
}

// Add other potential assertion-related functions here if needed in the future.
// For example, you might have more complex panic routines that save more state.