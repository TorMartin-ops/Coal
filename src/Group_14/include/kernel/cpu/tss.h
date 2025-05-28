#ifndef TSS_H
#define TSS_H

#include <kernel/core/types.h>  // For uint32_t and bool

// Structure of a Task State Segment (TSS)
typedef struct tss_entry {
    uint32_t prev_tss;   // The previous TSS - used with hardware task-switching
    uint32_t esp0;       // The stack pointer to load when changing to kernel mode
    uint32_t ss0;        // The stack segment to load when changing to kernel mode
    uint32_t esp1;       // The stack pointer to load when changing to ring 1
    uint32_t ss1;        // The stack segment to load when changing to ring 1
    uint32_t esp2;       // The stack pointer to load when changing to ring 2
    uint32_t ss2;        // The stack segment to load when changing to ring 2
    uint32_t cr3;        // The page directory pointer
    uint32_t eip;        // The instruction pointer
    uint32_t eflags;     // The flags register
    uint32_t eax;        // General purpose registers
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;         // Segment registers
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;        // The Local Descriptor Table
    uint16_t trap;       // Trap flag (bit 0 only, for task switching)
    uint16_t iomap_base; // The I/O Map Base Address Field (in TSS's)
} __attribute__((packed)) tss_entry_t;

// Initialize the TSS
void tss_init(void);

// Set the kernel stack pointer stored in the TSS
void tss_set_kernel_stack(uint32_t stack);

// Verify that the TSS esp0 value is reasonable
bool tss_debug_check_esp0(void);

// Get the current kernel stack pointer from the TSS <<< ADDED
uint32_t tss_get_esp0(void);

#define TSS_SELECTOR 0x28

#endif // TSS_H