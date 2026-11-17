#ifndef IDT_H
#define IDT_H

// Include dependencies FIRST
#include <kernel/cpu/isr_frame.h>
#include <kernel/lib/port_io.h>   // For outb, PIC defines below
#include <kernel/core/types.h>     // For uintN_t types

// --- Constants ---
#define IDT_ENTRIES 256
#define PF_VECTOR      14

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

// --- IRQ Vectors (derived from PIC start vectors) ------
#define IRQ0_VECTOR  (PIC1_START_VECTOR + 0)  // PIT Timer (Vector 32)
#define IRQ1_VECTOR  (PIC1_START_VECTOR + 1)  // Keyboard (Vector 33)
#define IRQ2_VECTOR  (PIC1_START_VECTOR + 2)  // Cascade (Vector 34)
#define IRQ3_VECTOR  (PIC1_START_VECTOR + 3)  // COM2 (Vector 35)
#define IRQ4_VECTOR  (PIC1_START_VECTOR + 4)  // COM1 (Vector 36)
#define IRQ5_VECTOR  (PIC1_START_VECTOR + 5)  // LPT2 (Vector 37)
#define IRQ6_VECTOR  (PIC1_START_VECTOR + 6)  // Floppy Disk (Vector 38)
#define IRQ7_VECTOR  (PIC1_START_VECTOR + 7)  // LPT1 / Spurious (Vector 39)
#define IRQ8_VECTOR  (PIC2_START_VECTOR + 0)  // RTC (Vector 40)
#define IRQ9_VECTOR  (PIC2_START_VECTOR + 1)  // Free / ACPI SCI (Vector 41)
#define IRQ10_VECTOR (PIC2_START_VECTOR + 2)  // Free / SCSI / Network (Vector 42)
#define IRQ11_VECTOR (PIC2_START_VECTOR + 3)  // Free / SCSI / Network (Vector 43)
#define IRQ12_VECTOR (PIC2_START_VECTOR + 4)  // PS/2 Mouse (Vector 44)
#define IRQ13_VECTOR (PIC2_START_VECTOR + 5)  // FPU / Coprocessor (Vector 45)
#define IRQ14_VECTOR (PIC2_START_VECTOR + 6)  // Primary ATA Hard Disk (Vector 46)
#define IRQ15_VECTOR (PIC2_START_VECTOR + 7)  // Secondary ATA Hard Disk (Vector 47)


// --- IDT Structure Definitions ---

// Defines an entry in the Interrupt Descriptor Table (a 64-bit gate takes 16 bytes).
struct idt_entry {
    uint16_t base_low;  // Handler address bits 0..15.
    uint16_t sel;       // Kernel code selector.
    uint8_t  ist;       // Interrupt stack table index (0: stay on the current stack).
    uint8_t  flags;     // Present, DPL and gate type (0x8E: ring 0 interrupt gate).
    uint16_t base_mid;  // Bits 16..31.
    uint32_t base_high; // Bits 32..63.
    uint32_t zero;
} __attribute__((packed));


// --- Interrupt Handler Function Pointer Type ---
typedef void (*int_handler_t)(isr_frame_t* frame);

// --- Structure to hold registered handler info ---
typedef struct interrupt_handler_info {
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

/** Prints the frame and halts: a kernel-mode exception nothing expected. */
void kernel_fault_panic(isr_frame_t *frame) __attribute__((noreturn));


/** The C handler on a vector, NULL if none. Lets a driver that shares an IRQ line call the one it replaced. */
int_handler_t get_int_handler(int num);

/** Number of interrupts taken on a vector (0..63) since boot. */
uint32_t irq_count(unsigned vector);

// Helper for optional delays (used in PIC init)
static inline void io_wait(void) {
    // Write to an unused port (often 0x80) as a delay
    // Ensure outb is declared (via port_io.h)
    outb(0x80, 0);
}

#endif // IDT_H
