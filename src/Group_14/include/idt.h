#ifndef IDT_H
#define IDT_H

// Include dependencies FIRST
// #include "paging.h" // <<< REMOVE: registers_t/isr_frame_t now defined in isr_frame.h
#include <isr_frame.h> // <<< ADD: Include the new frame definition
#include <port_io.h>             // For outb declaration before io_wait

// --- IDT Structure Definitions ---
// --- Constants ---
#define IDT_ENTRIES 256

// --- PIC Ports ---
#define PIC1         0x20      /* IO base address for master PIC */
#define PIC2         0xA0      /* IO base address for slave PIC */
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1 + 1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2 + 1)
#define PIC_EOI      0x20      /* End-of-interrupt command code */

// --- Add missing defines used in kernel.c --- (If needed elsewhere)
#ifndef PIC1_DAT
#define PIC1_DAT    PIC1_DATA
#endif
#ifndef PIC2_DAT
#define PIC2_DAT    PIC2_DATA
#endif
// --- End Added Defines ---


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


// --- Interrupt Handler Function Pointer Type ---
// *** UPDATED TYPE DEFINITION ***
// Define the function pointer type using the new isr_frame_t struct
typedef void (*int_handler_t)(isr_frame_t* frame);


// --- Structure to hold registered handler info ---
// Uses the updated int_handler_t type definition implicitly
typedef struct interrupt_handler_info {
    int           num;      // Interrupt number
    int_handler_t handler;  // Pointer to the C handler function
    void* data;     // Optional data pointer for the handler
} interrupt_handler_info_t; // Renamed type in idt.c, keep consistent if used elsewhere


// --- Public Function Prototypes ---

/**
 * @brief Initializes the IDT and PICs.
 */
void idt_init(void);

/**
 * @brief Registers a C handler function for a specific interrupt number.
 * *** UPDATED DECLARATION ***
 * @param num The interrupt number (0-255).
 * @param handler Pointer to the C handler function (using isr_frame_t*).
 * @param data Optional pointer to pass to the handler.
 */
void register_int_handler(int num, void (*handler)(isr_frame_t*), void* data);


// Helper for optional delays (used in PIC init)
static inline void io_wait(void) {
    outb(0x80, 0); // Write to an unused port (often 0x80) as a delay
}

#endif // IDT_H