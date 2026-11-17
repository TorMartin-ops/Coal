#include <kernel/cpu/gdt.h>
#include <kernel/cpu/percpu.h>
#include <kernel/lib/string.h>

/* Flat 64-bit segments (base and limit are ignored in long mode). */
#define SEG_KERNEL_CODE 0x00AF9A000000FFFFull
#define SEG_KERNEL_DATA 0x00CF92000000FFFFull
#define SEG_USER_CODE32 0x00CFFA000000FFFFull
#define SEG_USER_DATA   0x00CFF2000000FFFFull
#define SEG_USER_CODE64 0x00AFFA000000FFFFull

struct __attribute__((packed)) gdt_ptr {
    uint16_t limit;
    uint64_t base;
};

/* A 64-bit TSS descriptor takes two GDT slots. */
static void set_tss_descriptor(uint64_t *slot, const struct tss *tss) {
    uint64_t base = (uint64_t)tss, limit = sizeof(*tss) - 1;

    slot[0] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
              (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    slot[1] = base >> 32;
}

void gdt_init(struct percpu *cpu) {
    struct gdt_ptr gp = { sizeof(cpu->gdt) - 1, (uint64_t)cpu->gdt };

    cpu->gdt[0] = 0;
    cpu->gdt[1] = SEG_KERNEL_CODE;
    cpu->gdt[2] = SEG_KERNEL_DATA;
    cpu->gdt[3] = SEG_USER_CODE32;
    cpu->gdt[4] = SEG_USER_DATA;
    cpu->gdt[5] = SEG_USER_CODE64;
    set_tss_descriptor(&cpu->gdt[6], &cpu->tss);

    memset(&cpu->tss, 0, sizeof(cpu->tss));
    cpu->tss.iomap_base = sizeof(cpu->tss);             /* no I/O bitmap */
    for (unsigned i = 0; i < 3; i++)
        cpu->tss.ist[i] = (uint64_t)cpu->ist_stacks[i] + IST_STACK_SIZE;

    asm volatile("lgdt %0\n\t"
                 "pushq %1\n\t"
                 "leaq 1f(%%rip), %%rax\n\t"
                 "pushq %%rax\n\t"
                 "lretq\n"
                 "1:\n\t"
                 "movw %2, %%ax\n\t"
                 "movw %%ax, %%ds\n\t"
                 "movw %%ax, %%es\n\t"
                 "movw %%ax, %%ss\n\t"
                 "xorl %%eax, %%eax\n\t"
                 "movw %%ax, %%fs\n\t"
                 "movw %%ax, %%gs\n\t"
                 "movw %3, %%ax\n\t"
                 "ltr %%ax"
                 : : "m"(gp), "i"(KERNEL_CODE_SELECTOR), "i"(KERNEL_DATA_SELECTOR), "i"(GDT_TSS_SELECTOR)
                 : "rax", "memory");
}
