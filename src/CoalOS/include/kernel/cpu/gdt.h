#ifndef GDT_H
#define GDT_H

#include <kernel/core/types.h>

// Selector layout: 0 null, 1 kernel code, 2 kernel data, 3 user code,
// 4 user data, 5 TSS, 6 TLS (set_thread_area).
#define KERNEL_CODE_SELECTOR  0x08
#define KERNEL_DATA_SELECTOR  0x10
#define GDT_USER_CODE_SELECTOR (0x18 | 3)   // RPL 3
#define GDT_USER_DATA_SELECTOR (0x20 | 3)
#define GDT_TSS_SELECTOR      0x28
#define GDT_TLS_SELECTOR      (0x30 | 3)

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   // flags in the high nibble, limit bits 16-19 in the low
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/** Load the GDT and the TSS. */
void gdt_init(void);

/** Point the TLS descriptor (GDT_TLS_SELECTOR) at a user-space base address. */
void gdt_set_tls_base(uint32_t base, uint32_t limit_pages);

#endif // GDT_H
