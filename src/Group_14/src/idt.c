/**
 * idt.c
 *
 * A “world-class” IDT implementation for a 32-bit x86 kernel.
 * It handles both software ISRs (e.g. CPU exceptions) and hardware IRQs,
 * with PIC remapping and an optional C-level dispatcher for custom handlers.
 */

 #include "idt.h"
 #include "terminal.h"   // For printing messages in default handler
 #include "port_io.h"    // For outb/inb with PIC
 #include <libc/stdint.h>
 #include <stddef.h>
 
 /* 
    We define 256 IDT entries. Some are for CPU exceptions (0..31) 
    and others for hardware IRQs (remapped to 0x20..0x2F). 
 */
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr   idtp;
 
 /* 
    Optional: a table of custom C interrupt handlers. 
    If no handler is registered for a given vector, we’ll call default_int_handler(). 
 */
 static struct int_handler interrupt_handlers[IDT_ENTRIES];
 
 /* 
    We'll declare external references to the assembly stubs for both ISRs (0..31) 
    and hardware IRQs (0..15). 
    You only need to actually define the ones you use. 
    For example, if you have isr0..isr2, plus irq0..irq15.
 */
 extern void isr0();
 extern void isr1();
 extern void isr2();
 // ... isr3..isr31 if you want all CPU exceptions
 
 extern void irq0();
 extern void irq1();
 extern void irq2();
 extern void irq3();
 extern void irq4();
 extern void irq5();
 extern void irq6();
 extern void irq7();
 extern void irq8();
 extern void irq9();
 extern void irq10();
 extern void irq11();
 extern void irq12();
 extern void irq13();
 extern void irq14();
 extern void irq15();
 
 /* 
    PIC I/O ports and commands. For a standard 8259 PIC, we must remap 
    from the original vectors (0x08..0x0F) to 0x20..0x2F, avoiding conflicts 
    with CPU exceptions.
 */
 #define PIC1_COMMAND 0x20
 #define PIC1_DATA    0x21
 #define PIC2_COMMAND 0xA0
 #define PIC2_DATA    0xA1
 
 /*
    A small helper to load the IDT pointer using lidt instruction.
    We'll build the idtp struct, then pass to idt_load().
 */
 static inline void idt_load(struct idt_ptr* ptr)
 {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }
 
 /**
  * idt_set_gate
  *
  * Utility to fill in an IDT entry.
  *  - num: the IDT vector index (0..255)
  *  - base: the address of the handler function (e.g. isr0, irq0, etc.)
  *  - sel: the GDT selector (e.g. 0x08 for kernel code segment)
  *  - flags: type/attributes (0x8E = present, ring0, 32-bit interrupt gate)
  */
 static void idt_set_gate(int num, uint32_t base, uint16_t sel, uint8_t flags)
 {
     idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
     idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
 
     idt_entries[num].sel   = sel;
     idt_entries[num].null  = 0;     // must be zero
     idt_entries[num].flags = flags; // e.g. 0x8E
 }
 
 /**
  * pic_remap
  *
  * Remaps the Master and Slave PIC so that IRQs 0..15 map to IDT vectors 0x20..0x2F.
  */
 static void pic_remap(void)
 {
     // Save current interrupt masks
     uint8_t mask1 = inb(PIC1_DATA);
     uint8_t mask2 = inb(PIC2_DATA);
 
     // Start init (cascade mode)
     outb(PIC1_COMMAND, 0x11);
     outb(PIC2_COMMAND, 0x11);
 
     // ICW2: master offset=0x20, slave offset=0x28
     outb(PIC1_DATA, 0x20);
     outb(PIC2_DATA, 0x28);
 
     // ICW3: tell Master PIC that slave is at IRQ2, 
     //       and tell Slave PIC its cascade identity.
     outb(PIC1_DATA, 0x04);
     outb(PIC2_DATA, 0x02);
 
     // ICW4: 8086 mode
     outb(PIC1_DATA, 0x01);
     outb(PIC2_DATA, 0x01);
 
     // Restore saved masks
     outb(PIC1_DATA, mask1);
     outb(PIC2_DATA, mask2);
 }
 
 /**
  * default_int_handler
  *
  * If no handler is registered for a particular vector, we call this.
  * It just prints "[IDT] Unhandled interrupt." 
  * but you can expand with more info or a panic for CPU exceptions.
  */
 void default_int_handler(void* data)
 {
     terminal_write("[IDT] Unhandled interrupt.\n");
 }
 
 /**
  * register_int_handler
  *
  * Provide a custom C handler for interrupt vector 'num'.
  * The pointer 'data' can be used to store context or state if needed.
  */
 void register_int_handler(int num, void (*handler)(void*), void* data)
 {
     interrupt_handlers[num].num     = (uint16_t)num;
     interrupt_handlers[num].handler = handler;
     interrupt_handlers[num].data    = data;
 }
 
 /**
  * send_eoi
  *
  * When we finish handling an IRQ, we must send End-Of-Interrupt 
  * to the PIC(s) to let them know we're done. If it came from the 
  * slave PIC (IRQ >= 8), we must send EOI to both PIC2 and PIC1.
  */
 static void send_eoi(int irq_number)
 {
     if (irq_number >= 8) {
         outb(PIC2_COMMAND, 0x20);
     }
     outb(PIC1_COMMAND, 0x20);
 }
 
 /**
  * int_handler
  *
  * Called by the assembly stubs (isrX or irqX). 'num' is the interrupt vector.
  * We check if there's a registered handler; if not, call default_int_handler().
  * If it's an IRQ (0x20..0x2F), send EOI.
  */
 void int_handler(int num)
 {
     // Call custom C handler if present
     if (interrupt_handlers[num].handler) {
         interrupt_handlers[num].handler(interrupt_handlers[num].data);
     } else {
         terminal_write("Interrupt fired: ");
         // optionally print 'num' as a decimal or hex
         terminal_write("\n");
         default_int_handler(NULL);
     }
 
     // If this is an IRQ (vectors 32..47), we must send EOI
     if (num >= 32 && num <= 47) {
         send_eoi(num - 32); 
     }
 }
 
 /**
  * idt_init
  *
  *  1) Fill all 256 IDT entries with default gates
  *  2) Remap the PIC
  *  3) Install your ISR stubs for CPU exceptions (0..31) if you have them
  *  4) Install your IRQ stubs for hardware interrupts (0..15) => 0x20..0x2F
  *  5) Load the IDT with lidt
  */
 void idt_init(void)
 {
     // Prepare the IDT pointer
     idtp.limit = (uint16_t)(sizeof(idt_entries) - 1);
     idtp.base  = (uint32_t)&idt_entries[0];
 
     // Zero or default each entry
     for (int i = 0; i < IDT_ENTRIES; i++) {
         idt_set_gate(i, 0, 0x08, 0x8E); // 0x08 is kernel code segment
         interrupt_handlers[i].handler = 0;
         interrupt_handlers[i].data    = 0;
     }
 
     // Remap PIC so IRQ0..IRQ15 => 0x20..0x2F
     pic_remap();
 
     // If you have ISRs 0..31, set them here. For example:
     idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
     idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
     idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);
     // ... up to isr31 if you have them
 
     // IRQ0..IRQ15 => vectors 32..47
     idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
     idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
     idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
     idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
     idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
     idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
     idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
     idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
     idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
     idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
     idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
     idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
     idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
     idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
     idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
     idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);
 
     // Finally, load the IDT
     idt_load(&idtp);
 }
 