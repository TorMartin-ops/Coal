#pragma once
#ifndef TSS_H
#define TSS_H

#include "libc/stddef.h"
#include "libc/stdint.h"
#include "libc/stdbool.h"

/**
 * @brief Task State Segment (TSS) structure for 32-bit x86.
 *
 * This structure is aligned on a 4-byte boundary to ensure proper access.
 */
typedef struct tss_entry {
    uint32_t prev_tss;          // Unused, but must be zero.
    uint32_t esp0;              // Stack pointer to load when changing to kernel mode.
    uint32_t ss0;               // Stack segment to load when changing to kernel mode.
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt_segment_selector;
    uint16_t trap;
    uint16_t iomap_base;        // Offset to I/O permission bitmap.
} __attribute__((packed, aligned(4))) tss_entry_t;

/**
 * Initializes the Task State Segment (TSS).
 */
void tss_init(void);

/**
 * Updates the kernel stack pointer (ESP0) in the TSS.
 *
 * @param stack The new kernel stack pointer.
 */
void tss_set_kernel_stack(uint32_t stack);

#endif // TSS_H
