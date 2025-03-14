#include "gdt.h"

// Declare GDT with 3 entries: NULL, Code, Data
struct gdt_entry gdt[3];
struct gdt_ptr gdt_pointer;

// Helper function to set a GDT entry
void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;

    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = (limit >> 16) & 0x0F;

    gdt[index].granularity |= (granularity & 0xF0);
    gdt[index].access = access;
}

// Load GDT function (defined in assembly)
extern void gdt_flush(uint32_t gdt_ptr);

// Initialize the GDT
void gdt_init() {
    gdt_pointer.limit = (sizeof(gdt) - 1);
    gdt_pointer.base = (uint32_t)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);                // NULL segment (required)
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment

    gdt_flush((uint32_t)&gdt_pointer);
}
