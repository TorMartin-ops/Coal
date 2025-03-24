/**
 * gdt.c
 *
 * A robust Global Descriptor Table (GDT) setup for a 32-bit x86 OS.
 * Defines three segments:
 *   1) Null Segment (required)
 *   2) Code Segment (ring 0, 4 GiB base/limit)
 *   3) Data Segment (ring 0, 4 GiB base/limit)
 *
 * Also invokes gdt_flush (assembly) to load the new GDT and update segment registers.
 */

 #include <libc/stdint.h>
 #include "gdt.h"
 
 // gdt_flush is an assembly routine that performs "lgdt [gdt_ptr]"
 // and reloads segment registers (defined in gdt_flush.asm).
 extern void gdt_flush(uint32_t);
 
 // We'll define three entries: null, code, and data.
 static struct gdt_entry gdt[3];
 static struct gdt_ptr   gp;
 
 /**
  * gdt_set_gate
  *
  * Helper to fill a GDT entry with base/limit/access flags.
  * For a “flat” segment spanning all memory (0..4GiB), pass limit=0xFFFFFFFF.
  *
  * @param idx    Index in the GDT (0..2 in our case)
  * @param base   Segment base address
  * @param limit  Segment limit
  * @param access Access byte (e.g., 0x9A for code, 0x92 for data)
  * @param gran   Granularity byte (often 0xCF for 4KiB pages + 32-bit op size)
  */
 static void gdt_set_gate(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
 {
     gdt[idx].base_low    = (uint16_t)(base & 0xFFFF);
     gdt[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
     gdt[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
 
     gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
     gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F);
 
     // Combine top nibble of limit with “gran” flags (page granularity, 32-bit, etc.)
     gdt[idx].granularity |= (gran & 0xF0);
 
     gdt[idx].access = access;
 }
 
 /**
  * gdt_init
  *
  * Sets up our three-segment GDT:
  *   0) Null Segment
  *   1) Code Segment (0..4GiB, ring0)
  *   2) Data Segment (0..4GiB, ring0)
  *
  * Then calls gdt_flush() to load and activate the new GDT.
  */
 void gdt_init(void)
 {
     // Prepare GDT pointer
     gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
     gp.base  = (uint32_t)&gdt;
 
     // 0) Null Segment
     gdt_set_gate(0, 0, 0, 0, 0);
 
     // 1) Code Segment: base=0, limit=4GiB, access=0x9A, gran=0xCF
     //    0x9A => present, ring0, executable, read, accessed=0
     //    0xCF => granularity=4KiB, 32-bit mode
     gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
 
     // 2) Data Segment: base=0, limit=4GiB, access=0x92, gran=0xCF
     //    0x92 => present, ring0, not executable, writable, accessed=0
     gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
 
     // Flush the GDT into the CPU using our assembly stub.
     gdt_flush((uint32_t)&gp);
 }
 