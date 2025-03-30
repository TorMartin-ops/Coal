#include "tss.h"
#include "terminal.h"

#ifndef UINTPTR_MAX
typedef unsigned int uintptr_t;
#endif

extern void tss_flush(uint32_t selector);  // Assembly routine to load TSS.

tss_entry_t tss;

void tss_init(void) {
    // Zero out the TSS.
    uint32_t *tss_ptr = (uint32_t *)&tss;
    for (size_t i = 0; i < sizeof(tss_entry_t) / sizeof(uint32_t); i++) {
        tss_ptr[i] = 0;
    }

    tss.ss0 = 0x10;   // Kernel data segment selector.
    tss.esp0 = 0;     // Will be set later via tss_set_kernel_stack.
    tss.iomap_base = sizeof(tss_entry_t); // No I/O bitmap.
    tss.ldt_segment_selector = 0; // Not used.

    // Load the TSS.
    // Assuming the TSS descriptor is at GDT index 5, selector = 5*8.
    tss_flush(5 * 8);

    terminal_write("TSS initialized.\n");
}

void tss_set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}
