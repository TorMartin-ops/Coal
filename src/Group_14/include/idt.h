#ifndef IDT_H
#define IDT_H

#include "types.h"

#define IDT_ENTRIES 256

/* Each IDT entry for 32-bit protected mode. */
struct idt_entry {
    uint16_t base_low;  // lower 16 bits of the ISR's address
    uint16_t sel;       // GDT selector (e.g. 0x08 for kernel code)
    uint8_t  null;      // must be zero
    uint8_t  flags;     // gate type, DPL, and present bit
    uint16_t base_high; // higher 16 bits of the ISR's address
} __attribute__((packed));

/* The structure passed to lidt. */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Optional structure for registering interrupt handlers in C. */
struct int_handler {
    uint16_t num;
    void (*handler)(void* data);
    void* data;
} __attribute__((packed));

/* Initialize the IDT. */
void idt_init(void);

/* Register a handler for interrupt/vector num. */
void register_int_handler(int num, void (*handler)(void*), void* data);

/* The central function called by ASM stubs. */
void int_handler(int num);

/* The fallback if no custom handler is registered. */
void default_int_handler(void* data);

#endif // IDT_H