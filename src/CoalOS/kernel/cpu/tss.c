#include <kernel/cpu/tss.h>
#include <kernel/cpu/gdt.h>
#include <kernel/lib/string.h>

tss_entry_t tss;

void tss_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DATA_SELECTOR;
    tss.iomap_base = sizeof(tss);   // no I/O bitmap
}

/** Kernel stack used when entering ring 0 from ring 3. */
void tss_set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}
