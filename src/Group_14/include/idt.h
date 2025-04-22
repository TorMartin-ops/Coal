#ifndef IDT_H
#define IDT_H

// Include dependencies FIRST
#include <isr_frame.h> // <<< ADDED: Include the definition for isr_frame_t
#include <port_io.h>   // For outb, PIC defines below
#include <types.h>     // For uintN_t types

// --- Constants ---
#define IDT_ENTRIES 256

// --- PIC Ports ---
#define PIC1         0x20      /* IO base address for master PIC */
#define PIC2         0xA0      /* IO base address for slave PIC */
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1+1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2+1)
#define PIC_EOI      0x20      /* End-of-interrupt command code */

// PIC Remapping Vector Offsets
#define PIC1_START_VECTOR 0x20 // IRQ 0-7 map to 32-39
#define PIC2_START_VECTOR 0x28 // IRQ 8-15 map to 40-47

// Aliases for DATA ports if used elsewhere
#define PIC1_DAT PIC1_DATA
#define PIC2_DAT PIC2_DATA

// --- IDT Structure Definitions ---

// Defines an entry in the Interrupt Descriptor Table.
struct idt_entry {
    uint16_t base_low;  // Lower 16 bits of the handler function address.
    uint16_t sel;       // Kernel segment selector (usually 0x08 for code).
    uint8_t  null;      // Always zero.
    uint8_t  flags;     // Type and attributes flags (e.g., 0x8E for 32-bit interrupt gate).
    uint16_t base_high; // Upper 16 bits of the handler function address.
} __attribute__((packed));

// Defines the structure for the IDTR register (used with lidt).
struct idt_ptr {
    uint16_t limit; // Size of the IDT minus 1.
    uint32_t base;  // Base address of the IDT entries array.
} __attribute__((packed));


// --- Interrupt Handler Function Pointer Type ---
// <<< FIXED: Use isr_frame_t from isr_frame.h >>>
typedef void (*int_handler_t)(isr_frame_t* frame);

// --- Structure to hold registered handler info ---
typedef struct interrupt_handler_info {
    int           num;      // Interrupt number
    int_handler_t handler;  // Pointer to the C handler function
    void* data;     // Optional data pointer for the handler
} interrupt_handler_info_t;


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
void register_int_handler(int num, int_handler_t handler, void* data);


// Helper for optional delays (used in PIC init)
static inline void io_wait(void) {
    // Write to an unused port (often 0x80) as a delay
    // Ensure outb is declared (via port_io.h)
    outb(0x80, 0);
}

#endif // IDT_H