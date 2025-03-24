#include "idt.h"
#include "libc/stdmemory.h"   // For memset if you have it
#include "terminal.h"         // For printing in default_int_handler

/* 256 IDT entries + pointer structure. */
static struct idt_entry idt_entries[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* Optional array of C-level interrupt handlers. */
static struct int_handler interrupt_handlers[IDT_ENTRIES];

/* These come from isr_stubs.asm (or a similar .asm file) */
extern void isr0();
extern void isr1();
extern void isr2();

/* Inline function to load IDT pointer. */
static inline void idt_load(struct idt_ptr* idtp) {
    __asm__ volatile ("lidt (%0)" : : "r" (idtp));
}

/* A helper to set one IDT entry (vector). */
static void idt_set_gate(int32_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);

    idt_entries[num].sel   = sel;
    idt_entries[num].null  = 0;    // must be zero
    idt_entries[num].flags = flags;
}

/* Default handler: if no custom interrupt handler is registered. */
void default_int_handler(void* data) {
    terminal_write("[IDT] Unhandled interrupt.\n");
}

/* Register a custom handler for a specific interrupt vector. */
void register_int_handler(int num, void (*handler)(void*), void* data) {
    interrupt_handlers[num].num     = (uint16_t) num;
    interrupt_handlers[num].handler = handler;
    interrupt_handlers[num].data    = data;
}

/* Called by your assembly stubs with the interrupt vector. */
void int_handler(int num) {
    if (interrupt_handlers[num].handler) {
        interrupt_handlers[num].handler(interrupt_handlers[num].data);
    } else {
        // For Task 2, you might want to show which interrupt fired:
        terminal_write("Interrupt fired: ");
        // If you have a function to print numbers, use it here. 
        // For a quick example:
        // terminal_write_dec(num);
        terminal_write("\n");
        default_int_handler(NULL);
    }
}

/* Initialize all 256 IDT entries and load the table. */
void idt_init(void) {
    /* IDT pointer setup. */
    idtp.limit = sizeof(idt_entries) - 1;
    idtp.base  = (uint32_t)&idt_entries[0];

    /* Option A: use memset if you have it. */
    // memset(idt_entries, 0, sizeof(idt_entries));

    /* Option B: do it manually. Initialize every entry to a default. */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0x08, 0x8E);  // 0x08 = GDT code segment, 0x8E = present 32-bit int gate
        interrupt_handlers[i].handler = 0;
        interrupt_handlers[i].data    = 0;
    }

    /* Specifically set the first three interrupts to our stubs. */
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);

    /* Finally, load the IDT. */
    idt_load(&idtp);
}