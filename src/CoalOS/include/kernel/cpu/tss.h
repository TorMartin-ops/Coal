#ifndef TSS_H
#define TSS_H

#include <kernel/core/types.h>

/* 64-bit task state segment: only the stack pointers matter. */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;      /* stack for entries from ring 3 */
    uint64_t reserved1;
    uint64_t ist[7];                /* interrupt stack table: 1 = #DF, 2 = NMI, 3 = #MC */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3

#endif // TSS_H
