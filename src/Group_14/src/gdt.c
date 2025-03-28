/**
 * gdt.c
 * Global Descriptor Table setup for a 32-bit x86 OS.
 */

 #include <libc/stdint.h>
 #include "gdt.h"
 
 // Assembly routine to load the GDT.
 extern void gdt_flush(uint32_t);
 
 // Define three GDT entries: null, code, and data.
 static struct gdt_entry gdt[3];
 static struct gdt_ptr gp;
 
 /**
  * gdt_set_gate
  * Sets a GDT entry with the given base, limit, access, and granularity.
  *
  * @param idx    Index in the GDT.
  * @param base   Base address.
  * @param limit  Segment limit.
  * @param access Access flags.
  * @param gran   Granularity flags.
  */
 static void gdt_set_gate(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
 {
     gdt[idx].base_low    = (uint16_t)(base & 0xFFFF);
     gdt[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
     gdt[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
     gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
     gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F);
     gdt[idx].granularity |= (gran & 0xF0);
     gdt[idx].access = access;
 }
 
 /**
  * gdt_init
  * Initializes the GDT with a null segment, a code segment, and a data segment,
  * then flushes the new GDT to update the CPU's segment registers.
  */
 void gdt_init(void)
 {
     gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
     gp.base  = (uint32_t)&gdt;
 
     gdt_set_gate(0, 0, 0, 0, 0);                         // Null segment
     gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);           // Code segment: 0-4GiB, ring0, executable, read
     gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);           // Data segment: 0-4GiB, ring0, writable
 
     gdt_flush((uint32_t)&gp);
 }
 