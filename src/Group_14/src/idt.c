/**
 * idt.c – Complete IDT implementation for UiAOS
 * Includes Double Fault Handler registration.
 * Adds #TS (10) and #SS (12) handlers.
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
 extern void isr8();  // Double Fault
 extern void isr10(); // <<< Invalid TSS
 extern void isr11(); // Segment Not Present
 extern void isr12(); // <<< Stack Segment Fault
 extern void isr13(); // General Protection Fault
 extern void isr14(); // Page Fault
 extern void isr16(); // x87 Floating Point
 extern void isr17(); // Alignment Check
 extern void isr18(); // Machine Check
 extern void isr19(); // SIMD Floating Point

 extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
 extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
 extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
 extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

 extern void syscall_handler_asm(); // Syscall handler

 //--------------------------------------------------------------------------------------------------
 //  Constants that describe where the kernel is linked in memory
 //--------------------------------------------------------------------------------------------------
 #define KERNEL_PHYS_BASE  0x00100000   // must match linker script
 #define KERNEL_VIRT_BASE  0xC0000000   // higher‑half base

 #define PHYS_TO_VIRT(p)   ( (uint32_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE )

 //--------------------------------------------------------------------------------------------------
 //  PIC ports / helpers (io_wait added for robustness)
 //--------------------------------------------------------------------------------------------------
 #define PIC1_CMD 0x20
 #define PIC1_DAT 0x21
 #define PIC2_CMD 0xA0
 #define PIC2_DAT 0xA1
 #define PIC_EOI  0x20

 // Simple delay for I/O operations
 static inline void io_wait(void) {
     // Port 0x80 is used for POST checkpoints by some BIOSes, safe for delays
     outb(0x80, 0);
 }

 static inline void pic_remap(void)
 {
     uint8_t m1 = inb(PIC1_DAT);
     uint8_t m2 = inb(PIC2_DAT);

     outb(PIC1_CMD, 0x11); // Start initialization sequence (ICW1)
     outb(PIC2_CMD, 0x11);
     io_wait();

     outb(PIC1_DAT, 0x20); // ICW2: Master PIC vector offset (map IRQ 0-7 to INT 32-39)
     outb(PIC2_DAT, 0x28); // ICW2: Slave PIC vector offset (map IRQ 8-15 to INT 40-47)
     io_wait();

     outb(PIC1_DAT, 0x04); // ICW3: Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
     outb(PIC2_DAT, 0x02); // ICW3: Tell Slave PIC its cascade identity (0000 0010)
     io_wait();

     outb(PIC1_DAT, 0x01); // ICW4: 8086/88 (MCS-80/85) mode
     outb(PIC2_DAT, 0x01); // ICW4: 8086/88 (MCS-80/85) mode
     io_wait();

     outb(PIC1_DAT, m1);   // Restore saved masks
     outb(PIC2_DAT, m2);
 }

 static inline void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
 {
     idt_entries[num].base_low  = base & 0xFFFF;
     idt_entries[num].base_high = (base >> 16) & 0xFFFF;
     idt_entries[num].sel       = sel;     // Kernel Code Segment selector (0x08)
     idt_entries[num].null      = 0;       // Must be zero
     idt_entries[num].flags     = flags;   // Type and attributes (e.g., 0x8E = Present, Ring 0, 32-bit Interrupt Gate)
 }

 static inline void lidt(struct idt_ptr* ptr)
 {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }

 //--------------------------------------------------------------------------------------------------
 //  Default / registration helpers
 //--------------------------------------------------------------------------------------------------
 void default_int_handler(registers_t* regs); // Forward declaration
 void int_handler(registers_t* regs);         // Forward declaration

 void register_int_handler(int num, int_handler_t handler, void* data)
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
     if (irq >= 8) { // If IRQ came from slave PIC
         outb(PIC2_CMD, PIC_EOI);
     }
     outb(PIC1_CMD, PIC_EOI); // Always send EOI to master PIC
 }

 // Common C handler called by assembly stubs
 void int_handler(registers_t* regs)
 {
     // Call registered handler if one exists
     if (interrupt_handlers[regs->int_no].handler != NULL) {
         interrupt_handlers[regs->int_no].handler(regs);
     } else {
         // Use default handler for unregistered interrupts/exceptions
         // Special case for Double Fault (vector 8) - handled in ASM to halt
         if (regs->int_no != 8) {
              default_int_handler(regs);
         }
         // If it was vector 8, the ASM handler already halted.
     }

     // Send End-of-Interrupt (EOI) signal to PICs for hardware IRQs (32-47)
     if (regs->int_no >= 32 && regs->int_no <= 47) {
         send_eoi(regs->int_no - 32);
     }
 }

 // Very cut‑down panic‑style default handler
 void default_int_handler(registers_t* r)
 {
     terminal_printf("\n*** Unhandled Interrupt/Exception ***\n");
     terminal_printf(" Vector: %u (0x%x)\n", r->int_no, r->int_no);
     terminal_printf(" ErrCode: %#lx\n", (unsigned long)r->err_code);
     terminal_printf(" EIP: %p\n", (void*)r->eip);
     terminal_printf(" CS:  %#lx\n", (unsigned long)r->cs);
     terminal_printf(" EFLAGS: %#lx\n", (unsigned long)r->eflags);
     // Optionally print CR2 for page faults if default handler catches #14
     if (r->int_no == 14) {
         uintptr_t cr2;
         asm volatile("mov %%cr2, %0" : "=r"(cr2));
         terminal_printf(" CR2 (Fault Addr): %p\n", (void*)cr2);
     }
     terminal_printf("-----------------------------------\n");
     terminal_write(" System Halted.\n");
     while (1) __asm__ volatile ("cli; hlt");
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

     pic_remap(); // Remap PIC IRQs to avoid conflicts with CPU exceptions

     // CPU exceptions (Ring 0 Interrupt Gates - 0x8E)
     idt_set_gate(0,  PHYS_TO_VIRT((uintptr_t)isr0),  0x08, 0x8E);
     idt_set_gate(1,  PHYS_TO_VIRT((uintptr_t)isr1),  0x08, 0x8E);
     idt_set_gate(2,  PHYS_TO_VIRT((uintptr_t)isr2),  0x08, 0x8E);
     idt_set_gate(3,  PHYS_TO_VIRT((uintptr_t)isr3),  0x08, 0x8E);
     idt_set_gate(4,  PHYS_TO_VIRT((uintptr_t)isr4),  0x08, 0x8E);
     idt_set_gate(5,  PHYS_TO_VIRT((uintptr_t)isr5),  0x08, 0x8E);
     idt_set_gate(6,  PHYS_TO_VIRT((uintptr_t)isr6),  0x08, 0x8E);
     idt_set_gate(7,  PHYS_TO_VIRT((uintptr_t)isr7),  0x08, 0x8E);
     idt_set_gate(8,  PHYS_TO_VIRT((uintptr_t)isr8),  0x08, 0x8E); // Double Fault
     idt_set_gate(10, PHYS_TO_VIRT((uintptr_t)isr10), 0x08, 0x8E); // <<< Invalid TSS
     idt_set_gate(11, PHYS_TO_VIRT((uintptr_t)isr11), 0x08, 0x8E); // Segment Not Present
     idt_set_gate(12, PHYS_TO_VIRT((uintptr_t)isr12), 0x08, 0x8E); // <<< Stack Segment Fault
     idt_set_gate(13, PHYS_TO_VIRT((uintptr_t)isr13), 0x08, 0x8E); // General Protection Fault
     idt_set_gate(14, PHYS_TO_VIRT((uintptr_t)isr14), 0x08, 0x8E); // Page Fault
     idt_set_gate(16, PHYS_TO_VIRT((uintptr_t)isr16), 0x08, 0x8E); // x87 Floating Point
     idt_set_gate(17, PHYS_TO_VIRT((uintptr_t)isr17), 0x08, 0x8E); // Alignment Check
     idt_set_gate(18, PHYS_TO_VIRT((uintptr_t)isr18), 0x08, 0x8E); // Machine Check
     idt_set_gate(19, PHYS_TO_VIRT((uintptr_t)isr19), 0x08, 0x8E); // SIMD Floating Point

     // PIC IRQs (INT 32-47, Ring 0 Interrupt Gates - 0x8E)
     idt_set_gate(32, PHYS_TO_VIRT((uintptr_t)irq0),  0x08, 0x8E);
     idt_set_gate(33, PHYS_TO_VIRT((uintptr_t)irq1),  0x08, 0x8E);
     idt_set_gate(34, PHYS_TO_VIRT((uintptr_t)irq2),  0x08, 0x8E);
     idt_set_gate(35, PHYS_TO_VIRT((uintptr_t)irq3),  0x08, 0x8E);
     idt_set_gate(36, PHYS_TO_VIRT((uintptr_t)irq4),  0x08, 0x8E);
     idt_set_gate(37, PHYS_TO_VIRT((uintptr_t)irq5),  0x08, 0x8E);
     idt_set_gate(38, PHYS_TO_VIRT((uintptr_t)irq6),  0x08, 0x8E);
     idt_set_gate(39, PHYS_TO_VIRT((uintptr_t)irq7),  0x08, 0x8E);
     idt_set_gate(40, PHYS_TO_VIRT((uintptr_t)irq8),  0x08, 0x8E);
     idt_set_gate(41, PHYS_TO_VIRT((uintptr_t)irq9),  0x08, 0x8E);
     idt_set_gate(42, PHYS_TO_VIRT((uintptr_t)irq10), 0x08, 0x8E);
     idt_set_gate(43, PHYS_TO_VIRT((uintptr_t)irq11), 0x08, 0x8E);
     idt_set_gate(44, PHYS_TO_VIRT((uintptr_t)irq12), 0x08, 0x8E);
     idt_set_gate(45, PHYS_TO_VIRT((uintptr_t)irq13), 0x08, 0x8E);
     idt_set_gate(46, PHYS_TO_VIRT((uintptr_t)irq14), 0x08, 0x8E);
     idt_set_gate(47, PHYS_TO_VIRT((uintptr_t)irq15), 0x08, 0x8E);

     // INT 0x80 syscall gate (Ring 3 Trap Gate - 0xEE)
     idt_set_gate(0x80, PHYS_TO_VIRT((uintptr_t)syscall_handler_asm), 0x08, 0xEE);
     terminal_printf("[IDT] Registered syscall handler at interrupt 0x80\n");

     // Load the IDT register
     lidt(&idtp);
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }