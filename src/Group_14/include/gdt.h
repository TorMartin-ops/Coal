#ifndef GDT_H
#define GDT_H

#include <libc/stdint.h>

// Structure for each GDT entry (8 bytes)
struct gdt_entry {
    uint16_t limit_low;   // First 16 bits of the segment limit
    uint16_t base_low;    // First 16 bits of the base
    uint8_t base_middle;  // Next 8 bits of the base
    uint8_t access;       // Access flags (defines privilege level, type, etc.)
    uint8_t granularity;  // Flags and upper limit
    uint8_t base_high;    // Last 8 bits of the base
} __attribute__((packed)); // Prevents compiler optimizations

// Structure to define the pointer to the GDT
struct gdt_ptr {
    uint16_t limit; // Size of GDT - 1
    uint32_t base;  // Address of the first GDT entry
} __attribute__((packed));

void gdt_init();

#endif // GDT_H