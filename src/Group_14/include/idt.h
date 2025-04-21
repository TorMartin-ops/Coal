#ifndef IDT_H
#define IDT_H

// Include dependencies FIRST
#include "paging.h"  // <<< Get registers_t definition ONLY from here
#include "port_io.h" // <<< Get outb declaration before io_wait

// --- IDT Structure Definitions ---

// Defines an entry in the Interrupt Descriptor Table.
struct idt_entry {
    uint16_t base_low;  // Lower 16 bits of the handler function address.
    uint16_t sel;       // Kernel segment selector (usually 0x08 for code).
    uint8_t  null;      // Always zero.
    uint8_t  flags;     // Type and attributes flags (e.g., 0x8E for 32-bit interrupt gate, P=1, DPL=0).
    uint16_t base_high; // Upper 16 bits of the handler function address.
} __attribute__((packed));

// Defines the structure for the IDTR register (used with lidt).
struct idt_ptr {
    uint16_t limit; // Size of the IDT minus 1.
    uint32_t base;  // Base address of the IDT entries array.
} __attribute__((packed));

// Number of entries in the IDT.
#define IDT_ENTRIES 256

// --- Register Context Structure ---
// Defined in paging.h (included above)
// typedef struct registers { ... } registers_t;

// --- Interrupt Handler Function Pointer Type ---
// Define the function pointer type AFTER registers_t is known
typedef void (*int_handler_t)(registers_t* regs);

// --- Structure to hold registered handler info ---
// Define the struct using the handler type
typedef struct interrupt_handler_info { // <<< RENAMED struct
    int           num;      // Interrupt number
    int_handler_t handler;  // Pointer to the C handler function
    void* data;     // Optional data pointer for the handler
} interrupt_handler_info_t; // <<< Typedef for the struct


// --- Public Function Prototypes ---

/**
 * @brief Initializes the IDT and PICs.
 */
void idt_init(void);

/**
 * @brief Registers a C handler function for a specific interrupt number.
 *
 * @param num The interrupt number (0-255).
 * @param handler Pointer to the C handler function (type int_handler_t).
 * @param data Optional pointer to pass to the handler.
 */
void register_int_handler(int num, int_handler_t handler, void* data); // Uses typedef


// Helper for optional delays (used in PIC init)
static inline void io_wait(void) {
    outb(0x80, 0); // Write to an unused port (often 0x80) as a delay
}

#endif // IDT_H