#ifndef GDT_H
#define GDT_H

#include <kernel/core/types.h>

struct percpu;

/* Selector layout: 0 null, 1 kernel code, 2 kernel data, 3 user code (32-bit, unused), 4 user data,
 * 5 user code (64-bit), 6-7 TSS. SYSCALL and SYSRET fix the order of 1, 2 and 3..5 (see syscall.c). */
#define KERNEL_CODE_SELECTOR   0x08
#define KERNEL_DATA_SELECTOR   0x10
#define GDT_USER_DATA_SELECTOR (0x20 | 3)   /* RPL 3 */
#define GDT_USER_CODE_SELECTOR (0x28 | 3)
#define GDT_TSS_SELECTOR       0x30

/** Builds and loads this CPU's GDT and TSS. Needs the per-CPU data (percpu_init()). */
void gdt_init(struct percpu *cpu);

#endif // GDT_H
