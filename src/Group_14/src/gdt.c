#include "gdt.h"
#include "tss.h"       // For TSS definitions and initialization.
#include "terminal.h"  // For debug output (optional)

#include "libc/stdint.h"

// External assembly routines.
extern void gdt_flush(uint32_t);
extern void tss_flush(uint32_t);

// Global TSS; defined and initialized in tss.c.
extern struct tss_entry tss;

// We now have 6 GDT entries: null, kernel code, kernel data, user code, user data, TSS.
static struct gdt_entry gdt_entries[6];
static struct gdt_ptr gp;

/**
 * @brief Sets an individual GDT entry.
 *
 * @param idx    Index in the GDT array.
 * @param base   Base address for the segment.
 * @param limit  Limit for the segment.
 * @param access Access flags.
 * @param gran   Granularity flags.
 */
static void gdt_set_gate(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[idx].base_low    = (uint16_t)(base & 0xFFFF);
    gdt_entries[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt_entries[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt_entries[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt_entries[idx].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt_entries[idx].access = access;
}

void gdt_init(void) {
    // Total number of entries: 6.
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)&gdt_entries;
    
    // Entry 0: Null descriptor.
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // Entry 1: Kernel code segment. Base 0, limit 4GB, ring 0.
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    
    // Entry 2: Kernel data segment. Base 0, limit 4GB, ring 0.
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    // Entry 3: User code segment. Base 0, limit 4GB, ring 3.
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    
    // Entry 4: User data segment. Base 0, limit 4GB, ring 3.
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    // Entry 5: TSS descriptor.
    // For a 32-bit TSS, the limit is sizeof(tss_entry)-1.
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = sizeof(struct tss_entry) - 1;
    // Access byte 0x89: present, type 9 (available 32-bit TSS), DPL 0.
    // Granularity: set lower 4 bits of limit (from limit) and 0x40 to indicate byte granularity.
    gdt_set_gate(5, base, limit, 0x89, 0x40);
    
    // Load the new GDT.
    gdt_flush((uint32_t)&gp);
    
    // Initialize and load the TSS.
    tss_init();
    // Selector for the TSS entry: index 5, descriptor in GDT.
    // The selector value is 5*8 (offset in GDT) with RPL 3 typically used for user mode.
    tss_flush(5 * 8);
    
    terminal_write("GDT and TSS initialized.\n");
}
