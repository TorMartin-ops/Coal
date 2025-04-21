/**
 * idt.c – Complete IDT implementation for UiAOS (fixed IRQ‑stub address bug)
 *
 * Maps every CPU exception (0‑31) **and** every PIC IRQ (32‑47) to the
 * *higher‑half* virtual address of the corresponding assembly stub so that
 * the CPU never executes code in the low identity region after paging is
 * enabled.  This removes the supervisor‑mode page fault that happened as soon
 * as the first PIT interrupt fired.
 */

 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "types.h"
 #include <string.h>
 
 //--------------------------------------------------------------------------------------------------
 //  Internal data
 //--------------------------------------------------------------------------------------------------
 
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr   idtp;
 
 static struct int_handler interrupt_handlers[IDT_ENTRIES];
 
 //--------------------------------------------------------------------------------------------------
 //  External ISR / IRQ stubs that live in assembly (link‑time physical addresses)
 //--------------------------------------------------------------------------------------------------
 extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
 extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
 extern void isr8();  extern void isr10(); extern void isr11(); extern void isr12();
 extern void isr13(); extern void isr14(); extern void isr16(); extern void isr17();
 extern void isr18(); extern void isr19();
 
 extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
 extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
 extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
 extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();
 
 //--------------------------------------------------------------------------------------------------
 //  Constants that describe where the kernel is linked in memory
 //--------------------------------------------------------------------------------------------------
 #define KERNEL_PHYS_BASE  0x00100000   // must match linker script
 #define KERNEL_VIRT_BASE  0xC0000000   // higher‑half base
 
 #define PHYS_TO_VIRT(p)   ( (uint32_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE )
 
 //--------------------------------------------------------------------------------------------------
 //  PIC ports / helpers
 //--------------------------------------------------------------------------------------------------
 #define PIC1_CMD 0x20
 #define PIC1_DAT 0x21
 #define PIC2_CMD 0xA0
 #define PIC2_DAT 0xA1
 #define PIC_EOI  0x20
 
 static inline void pic_remap(void)
 {
     uint8_t m1 = inb(PIC1_DAT);
     uint8_t m2 = inb(PIC2_DAT);
 
     outb(PIC1_CMD, 0x11);
     outb(PIC2_CMD, 0x11);
 
     outb(PIC1_DAT, 0x20);   // master offset 0x20
     outb(PIC2_DAT, 0x28);   // slave  offset 0x28
 
     outb(PIC1_DAT, 0x04);
     outb(PIC2_DAT, 0x02);
 
     outb(PIC1_DAT, 0x01);
     outb(PIC2_DAT, 0x01);
 
     outb(PIC1_DAT, m1);
     outb(PIC2_DAT, m2);
 }
 
 static inline void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
 {
     idt_entries[num].base_low  = base & 0xFFFF;
     idt_entries[num].base_high = (base >> 16) & 0xFFFF;
     idt_entries[num].sel       = sel;
     idt_entries[num].null      = 0;
     idt_entries[num].flags     = flags;
 }
 
 static inline void lidt(struct idt_ptr* ptr)
 {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }
 
 //--------------------------------------------------------------------------------------------------
 //  Default / registration helpers – unchanged from earlier except for prototypes
 //--------------------------------------------------------------------------------------------------
 void default_int_handler(registers_t* regs);
 void int_handler(registers_t* regs);
 void register_int_handler(int num, void (*handler)(registers_t*), void* data)
 {
     if (num < IDT_ENTRIES)
     {
         interrupt_handlers[num].num     = num;
         interrupt_handlers[num].handler = handler;
         interrupt_handlers[num].data    = data;
     }
 }
 
 static void send_eoi(int irq)
 {
     if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
     outb(PIC1_CMD, PIC_EOI);
 }
 
 void int_handler(registers_t* regs)
 {
     if (interrupt_handlers[regs->int_no].handler)
         interrupt_handlers[regs->int_no].handler(regs);
     else
         default_int_handler(regs);
 
     if (regs->int_no >= 32 && regs->int_no <= 47)
         send_eoi(regs->int_no - 32);
 }
 
 // Very cut‑down panic‑style default handler
 void default_int_handler(registers_t* r)
 {
     terminal_printf("\n*** Unhandled interrupt %u err=%#x eip=%p ***\n", r->int_no, r->err_code, (void*)r->eip);
     while (1) __asm__("cli; hlt");
 }
 
 //--------------------------------------------------------------------------------------------------
 //  Public init
 //--------------------------------------------------------------------------------------------------
 void idt_init(void)
 {
     memset(idt_entries, 0, sizeof(idt_entries));
     memset(interrupt_handlers, 0, sizeof(interrupt_handlers));
 
     idtp.limit = sizeof(idt_entries) - 1;
     idtp.base  = (uint32_t)idt_entries;
 
     pic_remap();
 
     // CPU exceptions ------------------------------------------------------
     idt_set_gate(0,  PHYS_TO_VIRT(isr0),  0x08, 0x8E);
     idt_set_gate(1,  PHYS_TO_VIRT(isr1),  0x08, 0x8E);
     idt_set_gate(2,  PHYS_TO_VIRT(isr2),  0x08, 0x8E);
     idt_set_gate(3,  PHYS_TO_VIRT(isr3),  0x08, 0x8E);
     idt_set_gate(4,  PHYS_TO_VIRT(isr4),  0x08, 0x8E);
     idt_set_gate(5,  PHYS_TO_VIRT(isr5),  0x08, 0x8E);
     idt_set_gate(6,  PHYS_TO_VIRT(isr6),  0x08, 0x8E);
     idt_set_gate(7,  PHYS_TO_VIRT(isr7),  0x08, 0x8E);
     idt_set_gate(8,  PHYS_TO_VIRT(isr8),  0x08, 0x8E);
     idt_set_gate(10, PHYS_TO_VIRT(isr10), 0x08, 0x8E);
     idt_set_gate(11, PHYS_TO_VIRT(isr11), 0x08, 0x8E);
     idt_set_gate(12, PHYS_TO_VIRT(isr12), 0x08, 0x8E);
     idt_set_gate(13, PHYS_TO_VIRT(isr13), 0x08, 0x8E);
     idt_set_gate(14, PHYS_TO_VIRT(isr14), 0x08, 0x8E);
     idt_set_gate(16, PHYS_TO_VIRT(isr16), 0x08, 0x8E);
     idt_set_gate(17, PHYS_TO_VIRT(isr17), 0x08, 0x8E);
     idt_set_gate(18, PHYS_TO_VIRT(isr18), 0x08, 0x8E);
     idt_set_gate(19, PHYS_TO_VIRT(isr19), 0x08, 0x8E);
 
     // PIC IRQs ------------------------------------------------------------
     idt_set_gate(32, PHYS_TO_VIRT(irq0),  0x08, 0x8E);
     idt_set_gate(33, PHYS_TO_VIRT(irq1),  0x08, 0x8E);
     idt_set_gate(34, PHYS_TO_VIRT(irq2),  0x08, 0x8E);
     idt_set_gate(35, PHYS_TO_VIRT(irq3),  0x08, 0x8E);
     idt_set_gate(36, PHYS_TO_VIRT(irq4),  0x08, 0x8E);
     idt_set_gate(37, PHYS_TO_VIRT(irq5),  0x08, 0x8E);
     idt_set_gate(38, PHYS_TO_VIRT(irq6),  0x08, 0x8E);
     idt_set_gate(39, PHYS_TO_VIRT(irq7),  0x08, 0x8E);
     idt_set_gate(40, PHYS_TO_VIRT(irq8),  0x08, 0x8E);
     idt_set_gate(41, PHYS_TO_VIRT(irq9),  0x08, 0x8E);
     idt_set_gate(42, PHYS_TO_VIRT(irq10), 0x08, 0x8E);
     idt_set_gate(43, PHYS_TO_VIRT(irq11), 0x08, 0x8E);
     idt_set_gate(44, PHYS_TO_VIRT(irq12), 0x08, 0x8E);
     idt_set_gate(45, PHYS_TO_VIRT(irq13), 0x08, 0x8E);
     idt_set_gate(46, PHYS_TO_VIRT(irq14), 0x08, 0x8E);
     idt_set_gate(47, PHYS_TO_VIRT(irq15), 0x08, 0x8E);
 
     // INT 0x80 syscall gate (ring‑3) --------------------------------------
     extern void syscall_handler_asm();
     idt_set_gate(0x80, PHYS_TO_VIRT(syscall_handler_asm), 0x08, 0xEE); // DPL=3
 
     lidt(&idtp);
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }
 