#include <kernel/cpu/tss.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/core/types.h>
#include <kernel/lib/string.h>  // for memset

// We define the actual TSS structure here.
tss_entry_t tss;

/**
 * tss_init - Zeroes out the TSS and sets up essential fields.
 *
 * This function does not load the TSS into TR; that is done
 * by calling tss_flush() AFTER the GDT is loaded.
 */
void tss_init(void) {
    // Clear all fields
    memset(&tss, 0, sizeof(tss_entry_t));

    // The kernel data segment selector (index=2 => 0x10).
    // This is used when we enter ring 0 from ring 3 or an interrupt.
    tss.ss0 = 0x10; // KERNEL_DATA_SELECTOR

    // Initial ESP0 is 0 (will be updated before use)
    tss.esp0 = 0;
    terminal_printf("[TSS] Initial ESP0 set to 0 (will be updated before use)\n");

    // No I/O bitmap => set base after TSS, so no extra bits.
    tss.iomap_base = sizeof(tss_entry_t);

    // We do NOT call tss_flush here (the GDT might not be loaded yet).
    terminal_write("TSS initialized.\n");
}

/**
 * tss_set_kernel_stack - Updates esp0 in the TSS
 *
 * Called by the kernel to set the top of the kernel stack
 * for ring 0 transitions.
 */
void tss_set_kernel_stack(uint32_t stack) {
    // Add verification when setting ESP0
    if (stack == 0) {
        terminal_printf("[TSS ERROR] Attempt to set ESP0 to zero!\n");
        // Optionally panic here or just return? Returning might hide bugs.
        // KERNEL_PANIC_HALT("Attempt to set ESP0 to zero!");
        return;
    }
    // Basic check: should be in higher half
    if (stack < 0xC0000000) {
        terminal_printf("[TSS WARNING] Setting ESP0 to non-kernel space address: %p\n",
                      (void*)(uintptr_t)stack);
    }
    tss.esp0 = stack;
    // terminal_printf("[TSS] ESP0 updated to %p\n", (void*)(uintptr_t)stack); // Reduce verbosity maybe
}

/**
 * tss_debug_check_esp0 - Verify that the TSS esp0 value is reasonable
 *
 * Returns true if ESP0 is non-zero and appears to be in kernel space
 */
bool tss_debug_check_esp0(void) {
    // Check that ESP0 is non-zero
    if (tss.esp0 == 0) {
        terminal_printf("[TSS Debug] ERROR: ESP0 is ZERO!\n");
        return false;
    }

    // Check that ESP0 is in kernel space (higher half)
    // Check if it's within a reasonable range (e.g., not just barely above C0000000)
    if (tss.esp0 < 0xC0100000) { // Adjusted lower bound check
        terminal_printf("[TSS Debug] ERROR: ESP0 (%p) is suspiciously low in kernel space!\n",
                      (void*)(uintptr_t)tss.esp0);
        return false;
    }

    terminal_printf("[TSS Debug] ESP0 looks valid: %p\n", (void*)(uintptr_t)tss.esp0);
    return true;
}

/**
 * tss_get_esp0 - Returns the current esp0 value from the TSS <<< ADDED
 */
uint32_t tss_get_esp0(void) {
    return tss.esp0;
}