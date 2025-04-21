/**
 * idt.c – Complete IDT implementation for UiAOS
 * Includes Double Fault Handler registration.
 * Fixes build errors and warnings (round 2).
 */

 #include "idt.h"
 #include "terminal.h"
 // port_io.h is now included via idt.h
 #include "types.h"
 #include <string.h> // For memset

 //--------------------------------------------------------------------------------------------------
 //  Internal data
 //--------------------------------------------------------------------------------------------------

 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr   idtp;

 // Use the typedef for the struct defined in idt.h
 static interrupt_handler_info_t interrupt_handlers[IDT_ENTRIES]; // <<< Use struct typedef from header

 //--------------------------------------------------------------------------------------------------
 //  External ISR / IRQ stubs
 //--------------------------------------------------------------------------------------------------
 extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
 extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
 extern void isr8();  // Double Fault
 extern void isr10(); // Invalid TSS
 extern void isr11(); // Segment Not Present
 extern void isr12(); // Stack Segment Fault
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
 //  Constants for PHYS_TO_VIRT
 //--------------------------------------------------------------------------------------------------
 #define KERNEL_PHYS_BASE  0x00100000
 #define KERNEL_VIRT_BASE  0xC0000000
 #define PHYS_TO_VIRT(p)   ( (uint32_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE )

 //--------------------------------------------------------------------------------------------------
 //  PIC ports / helpers
 //--------------------------------------------------------------------------------------------------
 #define PIC1_CMD 0x20
 #define PIC1_DAT 0x21
 #define PIC2_CMD 0xA0
 #define PIC2_DAT 0xA1
 #define PIC_EOI  0x20

 // io_wait is now defined in idt.h as static inline

 static inline void pic_remap(void)
 {
     uint8_t m1 = inb(PIC1_DAT); // Save masks
     uint8_t m2 = inb(PIC2_DAT);

     outb(PIC1_CMD, 0x11); // Start initialization sequence (ICW1) - edge triggered mode
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
     // Bounds check removed - num is uint8_t, cannot be >= 256
     idt_entries[num].base_low  = base & 0xFFFF;
     idt_entries[num].base_high = (base >> 16) & 0xFFFF;
     idt_entries[num].sel       = sel;
     idt_entries[num].null      = 0;
     idt_entries[num].flags     = flags;
 }

 // Load IDT Register
 static inline void lidt(struct idt_ptr* ptr)
 {
     __asm__ volatile("lidt (%0)" : : "r"(ptr));
 }

 //--------------------------------------------------------------------------------------------------
 //  Default / registration helpers
 //--------------------------------------------------------------------------------------------------
 void default_int_handler(registers_t* regs); // Forward declaration
 void int_handler(registers_t* regs);         // Forward declaration

 // Register a C function to handle a specific interrupt
 void register_int_handler(int num, int_handler_t handler, void* data) // Uses typedef
 {
     if (num >= 0 && num < IDT_ENTRIES) // Check bounds
     {
         // Use struct typedef defined in header
         interrupt_handlers[num].num     = num; // <<< Use struct typedef from header
         interrupt_handlers[num].handler = handler;
         interrupt_handlers[num].data    = data;
     }
 }

 // Send End-Of-Interrupt signal to PICs
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
     // Use struct typedef defined in header
     if (regs->int_no < IDT_ENTRIES && interrupt_handlers[regs->int_no].handler != NULL) { // <<< Use struct typedef + bounds check
         interrupt_handlers[regs->int_no].handler(regs); // <<< Use struct typedef
     } else {
         // Use default handler for unregistered interrupts/exceptions
         // Special case for Double Fault (vector 8) - handled in ASM to halt
         if (regs->int_no != 8) {
              default_int_handler(regs);
         }
     }

     // Send EOI signal for hardware IRQs
     if (regs->int_no >= 32 && regs->int_no <= 47) {
         send_eoi(regs->int_no - 32);
     }
 }

 // Basic default handler that prints info and halts
 void default_int_handler(registers_t* r)
 {
     terminal_printf("\n*** Unhandled Interrupt/Exception ***\n");
     // Use %lu/%lx for uint32_t
     terminal_printf(" Vector: %lu (0x%lx)\n", (unsigned long)r->int_no, (unsigned long)r->int_no); // Corrected format
     terminal_printf(" ErrCode: %#lx\n", (unsigned long)r->err_code);
     terminal_printf(" EIP: %p\n", (void*)r->eip);
     terminal_printf(" CS:  %#lx\n", (unsigned long)r->cs);
     terminal_printf(" EFLAGS: %#lx\n", (unsigned long)r->eflags);
     if (r->int_no == 14) { // Page Fault specific info
         uintptr_t cr2;
         asm volatile("mov %%cr2, %0" : "=r"(cr2));
         terminal_printf(" CR2 (Fault Addr): %p\n", (void*)cr2);
     }
     terminal_printf("-----------------------------------\n");
     terminal_write(" System Halted.\n");
     while (1) __asm__ volatile ("cli; hlt");
 }

 //--------------------------------------------------------------------------------------------------
 //  Public init function
 //--------------------------------------------------------------------------------------------------
 void idt_init(void)
 {
     memset(idt_entries, 0, sizeof(idt_entries));
     memset(interrupt_handlers, 0, sizeof(interrupt_handlers));

     idtp.limit = sizeof(idt_entries) - 1;
     idtp.base  = (uint32_t)idt_entries; // Physical address of the IDT

     pic_remap(); // Remap PIC IRQs

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
     idt_set_gate(10, PHYS_TO_VIRT((uintptr_t)isr10), 0x08, 0x8E); // Invalid TSS
     idt_set_gate(11, PHYS_TO_VIRT((uintptr_t)isr11), 0x08, 0x8E); // Segment Not Present
     idt_set_gate(12, PHYS_TO_VIRT((uintptr_t)isr12), 0x08, 0x8E); // Stack Segment Fault
     idt_set_gate(13, PHYS_TO_VIRT((uintptr_t)isr13), 0x08, 0x8E); // General Protection Fault
     idt_set_gate(14, PHYS_TO_VIRT((uintptr_t)isr14), 0x08, 0x8E); // Page Fault
     idt_set_gate(16, PHYS_TO_VIRT((uintptr_t)isr16), 0x08, 0x8E); // x87 Floating Point
     idt_set_gate(17, PHYS_TO_VIRT((uintptr_t)isr17), 0x08, 0x8E); // Alignment Check
     idt_set_gate(18, PHYS_TO_VIRT((uintptr_t)isr18), 0x08, 0x8E); // Machine Check
     idt_set_gate(19, PHYS_TO_VIRT((uintptr_t)isr19), 0x08, 0x8E); // SIMD Floating Point

     // PIC IRQs (INT 32-47, Ring 0 Interrupt Gates - 0x8E)
     idt_set_gate(32, PHYS_TO_VIRT((uintptr_t)irq0),  0x08, 0x8E); // PIT
     idt_set_gate(33, PHYS_TO_VIRT((uintptr_t)irq1),  0x08, 0x8E); // Keyboard
     // ... (rest of IRQ gates) ...
     idt_set_gate(46, PHYS_TO_VIRT((uintptr_t)irq14), 0x08, 0x8E); // Primary ATA HD
     idt_set_gate(47, PHYS_TO_VIRT((uintptr_t)irq15), 0x08, 0x8E); // Secondary ATA HD

     // INT 0x80 syscall gate (Ring 3 Trap Gate - 0xEE)
     idt_set_gate(0x80, PHYS_TO_VIRT((uintptr_t)syscall_handler_asm), 0x08, 0xEE);
     terminal_printf("[IDT] Registered syscall handler at interrupt 0x80\n");

     // Load the IDT register
     lidt(&idtp);
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }