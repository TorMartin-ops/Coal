#include "tss.h"
#include "terminal.h"
#include "libc/stdint.h"
#include "libc/stddef.h"
#include "libc/string.h"  // for memset

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
    tss.ss0 = 0x10;

    // The kernel stack pointer (esp0) is set later via tss_set_kernel_stack().
    tss.esp0 = 0;

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
    tss.esp0 = stack;
}
