#pragma once
#ifndef GDT_H
#define GDT_H

#include "libc/stdint.h"  // Provides uint32_t, etc.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Global Descriptor Table (GDT) entry structure.
 *
 * Each entry is 8 bytes long.
 */
struct gdt_entry {
    uint16_t limit_low;    // Lower 16 bits of the segment limit.
    uint16_t base_low;     // Lower 16 bits of the base address.
    uint8_t  base_middle;  // Next 8 bits of the base address.
    uint8_t  access;       // Access flags determine what ring this segment can be used in.
    uint8_t  granularity;  // Contains upper 4 bits of limit and flags.
    uint8_t  base_high;    // Highest 8 bits of the base address.
} __attribute__((packed));

/**
 * @brief Pointer to the GDT used by the LGDT instruction.
 */
struct gdt_ptr {
    uint16_t limit;  // Size of the GDT (in bytes) minus 1.
    uint32_t base;   // Base address of the first GDT entry.
} __attribute__((packed));

/**
 * @brief Initializes the Global Descriptor Table.
 *
 * The GDT is configured with the following segments:
 *   - Null descriptor.
 *   - Kernel code segment (ring 0).
 *   - Kernel data segment (ring 0).
 *   - User code segment (ring 3).
 *   - User data segment (ring 3).
 *   - TSS descriptor.
 *
 * After setting up the GDT, the function flushes it to update the CPU's segment registers
 * and loads the TSS.
 */
void gdt_init(void);

#ifdef __cplusplus
}
#endif

#endif // GDT_H
