/**
 * idt.c
 * IDT implementation for a 32-bit x86 kernel.
 * Handles CPU exceptions (vectors 0–31) and hardware IRQs (remapped to 32–47).
 */

 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "types.h"
 
 // IDT entries and pointer.
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr idtp;
 
 // Table of custom C interrupt handlers; if none is registered, default_int_handler is used.
 static struct int_handler interrupt_handlers[IDT_ENTRIES];
 
 // External ISR stubs for CPU exceptions.
 extern void isr0();
 extern void isr1();
 extern void isr2();
 // ... (others as needed)
 
 // External IRQ stubs for hardware interrupts.
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
 
 // PIC I/O ports.
 #define PIC1_COMMAND 0x20
 #define PIC1_DATA    0x21
 #define PIC2_COMMAND 0xA0
 #define PIC2_DATA    0xA1
 
 // Load the IDT pointer.
 static inline void idt_load(struct idt_ptr* ptr) {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }
 
 // Set an IDT entry.
 static void idt_set_gate(int num, uint32_t base, uint16_t sel, uint8_t flags) {
     idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
     idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
     idt_entries[num].sel   = sel;
     idt_entries[num].null  = 0;
     idt_entries[num].flags = flags;
 }
 
 // Remap the PIC so IRQs 0–15 are mapped to vectors 32–47.
 static void pic_remap(void) {
     uint8_t mask1 = inb(PIC1_DATA);
     uint8_t mask2 = inb(PIC2_DATA);
     
     outb(PIC1_COMMAND, 0x11);
     outb(PIC2_COMMAND, 0x11);
     
     outb(PIC1_DATA, 0x20);
     outb(PIC2_DATA, 0x28);
     
     outb(PIC1_DATA, 0x04);
     outb(PIC2_DATA, 0x02);
     
     outb(PIC1_DATA, 0x01);
     outb(PIC2_DATA, 0x01);
     
     outb(PIC1_DATA, mask1);
     outb(PIC2_DATA, mask2);
 }
 
 // Default interrupt handler.
 void default_int_handler(void* data) {
     terminal_write("[IDT] Unhandled interrupt.\n");
 }
 
 // Register a custom interrupt handler for vector 'num'.
 void register_int_handler(int num, void (*handler)(void*), void* data) {
     interrupt_handlers[num].num = (uint16_t)num;
     interrupt_handlers[num].handler = handler;
     interrupt_handlers[num].data = data;
 }
 
 // Send End-Of-Interrupt (EOI) for the given IRQ.
 static void send_eoi(int irq_number) {
     if (irq_number >= 8)
         outb(PIC2_COMMAND, 0x20);
     outb(PIC1_COMMAND, 0x20);
 }
 
 // C-level interrupt handler called by assembly stubs.
 void int_handler(int num) {
     if (interrupt_handlers[num].handler)
         interrupt_handlers[num].handler(interrupt_handlers[num].data);
     else {
         terminal_write("Interrupt fired: \n");
         default_int_handler(NULL);
     }
 
     if (num >= 32 && num <= 47)
         send_eoi(num - 32);
 }
 
 // Initialize the IDT.
 void idt_init(void) {
     idtp.limit = (uint16_t)(sizeof(idt_entries) - 1);
     idtp.base  = (uint32_t)&idt_entries[0];
 
     for (int i = 0; i < IDT_ENTRIES; i++) {
         idt_set_gate(i, 0, 0x08, 0x8E);
         interrupt_handlers[i].handler = 0;
         interrupt_handlers[i].data = 0;
     }
 
     pic_remap();
 
     // Install CPU exception handlers (vectors 0–31)
     idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
     idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
     idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);
     // ... (set other ISRs as needed)
 
     // Install hardware IRQ handlers (vectors 32–47)
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
 
     idt_load(&idtp);
 }
 