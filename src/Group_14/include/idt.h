#ifndef IDT_H
#define IDT_H

#include "types.h"
// Include paging.h for registers_t definition
#include "paging.h"

#define IDT_ENTRIES 256

/* IDT entry structure */
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  null;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

/* IDT pointer structure */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Structure for registering C handlers */
struct int_handler {
    uint16_t num;
    // *** Consistent signature using registers_t* ***
    void (*handler)(registers_t *regs);
    void* data; // Optional data pointer for handler
} __attribute__((packed));

/* Initialize the IDT */
void idt_init(void);

/* Register a handler for interrupt/vector num */
// *** Consistent signature ***
void register_int_handler(int num, void (*handler)(registers_t *regs), void* data);

/* C-level dispatcher called by ASM stubs */
// *** Consistent signature ***
void int_handler(registers_t *regs);

/* Default handler if no custom one is registered */
// *** Consistent signature ***
void default_int_handler(registers_t *regs);

#endif // IDT_H