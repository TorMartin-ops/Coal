/**
 * idt.c
 * IDT implementation for a 32-bit x86 kernel.
 */

 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "types.h"
 #include "paging.h" // Include for registers_t definition if needed elsewhere

 // IDT entries and pointer.
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr idtp;

 // Table of custom C interrupt handlers; if none is registered, default_int_handler is used.
 static struct int_handler interrupt_handlers[IDT_ENTRIES];

 // External ISR stubs for CPU exceptions.
 extern void isr0();  // DE
 extern void isr1();  // DB
 extern void isr2();  // NMI
 extern void isr3();  // BP
 extern void isr4();  // OF
 extern void isr5();  // BR
 extern void isr6();  // UD
 extern void isr7();  // NM
 extern void isr8();  // DF
 // 9 reserved
 extern void isr10(); // TS
 extern void isr11(); // NP
 extern void isr12(); // SS
 extern void isr13(); // GP
 extern void isr14(); // PF <- Page Fault handler stub
 // 15 reserved
 extern void isr16(); // MF
 extern void isr17(); // AC
 extern void isr18(); // MC
 extern void isr19(); // XF
 // ... add others as needed

 // External IRQ stubs for hardware interrupts.
 extern void irq0(); extern void irq1(); extern void irq2(); extern void irq3();
 extern void irq4(); extern void irq5(); extern void irq6(); extern void irq7();
 extern void irq8(); extern void irq9(); extern void irq10(); extern void irq11();
 extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

 // PIC I/O ports.
 #define PIC1_COMMAND 0x20
 #define PIC1_DATA    0x21
 #define PIC2_COMMAND 0xA0
 #define PIC2_DATA    0xA1
 #define PIC_EOI      0x20 // End-of-interrupt command code

 // Load the IDT pointer.
 static inline void idt_load(struct idt_ptr* ptr) {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }

 // Set an IDT entry.
 static void idt_set_gate(int num, uint32_t base, uint16_t sel, uint8_t flags) {
     if (num < 0 || num >= IDT_ENTRIES) return; // Bounds check
     idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
     idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
     idt_entries[num].sel   = sel;
     idt_entries[num].null  = 0; // Must be zero
     idt_entries[num].flags = flags; // Type and attributes (e.g., 0x8E = 32-bit Int Gate, Ring 0, Present)
 }

 // Remap the PIC so IRQs 0–15 are mapped to vectors 32–47.
 static void pic_remap(void) {
     // Save masks
     uint8_t mask1 = inb(PIC1_DATA);
     uint8_t mask2 = inb(PIC2_DATA);

     // Start initialization sequence (ICW1)
     outb(PIC1_COMMAND, 0x11); // Init, Expect ICW4
     outb(PIC2_COMMAND, 0x11); // Init, Expect ICW4

     // ICW2: Set interrupt vector offsets
     outb(PIC1_DATA, 0x20); // Master PIC vectors start at 32 (0x20)
     outb(PIC2_DATA, 0x28); // Slave PIC vectors start at 40 (0x28)

     // ICW3: Configure Master/Slave relationship
     outb(PIC1_DATA, 0x04); // Tell Master PIC that slave is at IRQ2 (0000 0100)
     outb(PIC2_DATA, 0x02); // Tell Slave PIC its cascade identity (0000 0010)

     // ICW4: Set mode (8086/88 mode)
     outb(PIC1_DATA, 0x01); // 8086/88 (MCS-80/85) mode
     outb(PIC2_DATA, 0x01); // 8086/88 (MCS-80/85) mode

     // Restore saved masks
     outb(PIC1_DATA, mask1);
     outb(PIC2_DATA, mask2);
 }

 // Default interrupt handler.
 void default_int_handler(registers_t *regs) {
     terminal_printf("[IDT] Unhandled Interrupt/Exception: %d (Error Code: 0x%x)\n", regs->int_no, regs->err_code);
     // In a real kernel, you might print more registers, panic, or kill the process
     terminal_write("System Halted.\n");
     asm volatile ("cli; hlt");
 }

 // Register a custom interrupt handler for vector 'num'.
 void register_int_handler(int num, void (*handler)(registers_t*), void* data) {
     if (num < 0 || num >= IDT_ENTRIES) return; // Bounds check
     // Store handler pointer. Note: 'data' parameter is not used in current setup.
     interrupt_handlers[num].num = (uint16_t)num;
     interrupt_handlers[num].handler = handler;
     interrupt_handlers[num].data = data; // Store data pointer if needed by handler
 }

 // Send End-Of-Interrupt (EOI) for the given IRQ number (0-15).
 static void send_eoi(int irq_number) {
     if (irq_number >= 8) {
         outb(PIC2_COMMAND, PIC_EOI); // Send EOI to Slave PIC
     }
     outb(PIC1_COMMAND, PIC_EOI); // Send EOI to Master PIC
 }

 // C-level interrupt handler called by assembly stubs.
 // It now receives the register context pointer.
 void int_handler(registers_t *regs) {
     // terminal_printf("Interrupt %d received, error code 0x%x\n", regs->int_no, regs->err_code); // Debug

     // Check if a custom handler is registered
     if (interrupt_handlers[regs->int_no].handler) {
         interrupt_handlers[regs->int_no].handler(regs); // Call custom handler
     } else {
         // No custom handler, use the default
         default_int_handler(regs);
     }

     // Send EOI for hardware interrupts (IRQ 0-15 map to vectors 32-47)
     if (regs->int_no >= 32 && regs->int_no <= 47) {
         send_eoi(regs->int_no - 32);
     }
 }

 // Initialize the IDT.
 void idt_init(void) {
     idtp.limit = (uint16_t)(sizeof(idt_entries) - 1);
     idtp.base  = (uint32_t)&idt_entries[0];

     // Clear IDT entries and handler table
     memset(idt_entries, 0, sizeof(idt_entries));
     memset(interrupt_handlers, 0, sizeof(interrupt_handlers));

     pic_remap(); // Remap PIC before setting gates

     // Install ISRs for CPU exceptions (0-31)
     // Using 0x8E: P=1, DPL=0 (Kernel), Type=0xE (32-bit Interrupt Gate)
     idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
     idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
     idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
     idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
     idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
     idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
     idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
     idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
     idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E); // Double Fault
     // ISR 9 is reserved
     idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E); // Invalid TSS
     idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E); // Segment Not Present
     idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E); // Stack-Segment Fault
     idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // General Protection Fault
     idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); // *** Page Fault ***
     // ISR 15 is reserved
     idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E); // x87 FPU Error
     idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E); // Alignment Check
     idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E); // Machine Check
     idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E); // SIMD FP Exception
     // ISRs 20-31 are reserved or architecture-specific

     // Install IRQ handlers (32-47)
     idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E); // PIT
     idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E); // Keyboard
     idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E); // Cascade
     idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E); // COM2
     idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E); // COM1
     idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E); // LPT2
     idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E); // Floppy Disk
     idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E); // LPT1 / Spurious
     idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E); // RTC
     idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E); // Free
     idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E); // Free
     idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E); // Free
     idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E); // PS/2 Mouse
     idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E); // FPU Coprocessor
     idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E); // Primary ATA Hard Disk
     idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E); // Secondary ATA Hard Disk

     // *** Register Page Fault C Handler ***
     register_int_handler(14, page_fault_handler, NULL);

     // Load the IDT
     idt_load(&idtp);
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }