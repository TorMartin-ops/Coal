/**
 * idt.c
 * IDT implementation for a 32-bit x86 kernel.
 */

 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "types.h"
 #include "paging.h"     // For registers_t
 #include "scheduler.h"  // For remove_current_task_with_code
 #include "process.h"    // *** Added include for get_current_process ***
 #include <string.h>    // *** Added include for memset ***

 // IDT entries and pointer.
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr idtp;

 // Table of custom C interrupt handlers.
 // The handler function pointer now correctly uses registers_t*
 static struct int_handler interrupt_handlers[IDT_ENTRIES];

 // External ISR/IRQ stubs (declarations remain the same)
 extern void isr0(); extern void isr1(); extern void isr2(); extern void isr3();
 extern void isr4(); extern void isr5(); extern void isr6(); extern void isr7();
 extern void isr8(); extern void isr10(); extern void isr11(); extern void isr12();
 extern void isr13(); extern void isr14(); extern void isr16(); extern void isr17();
 extern void isr18(); extern void isr19();
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
     if (num < 0 || num >= IDT_ENTRIES) return;
     idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
     idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
     idt_entries[num].sel   = sel;
     idt_entries[num].null  = 0;
     idt_entries[num].flags = flags;
 }

 // Remap the PIC (implementation remains the same)
 static void pic_remap(void) {
     uint8_t mask1 = inb(PIC1_DATA); uint8_t mask2 = inb(PIC2_DATA);
     outb(PIC1_COMMAND, 0x11); outb(PIC2_COMMAND, 0x11); // ICW1
     outb(PIC1_DATA, 0x20); outb(PIC2_DATA, 0x28);       // ICW2: Vector Offsets
     outb(PIC1_DATA, 0x04); outb(PIC2_DATA, 0x02);       // ICW3: Master/Slave config
     outb(PIC1_DATA, 0x01); outb(PIC2_DATA, 0x01);       // ICW4: 8086 mode
     outb(PIC1_DATA, mask1); outb(PIC2_DATA, mask2);    // Restore masks
 }

 // Default interrupt handler - Now terminates the process.
 // *** Changed signature to match idt.h ***
 void default_int_handler(registers_t *regs) {
    // Note: Interrupts should be disabled by the hardware upon entering the handler.

    // *** ADD CHECK FOR CURRENT PROCESS ***
    pcb_t* current_proc = get_current_process();

    if (current_proc == NULL) {
        // --- FATAL KERNEL EXCEPTION DURING INIT/IDLE ---
        // No process context exists, cannot terminate gracefully. This is critical.
        terminal_printf("\n--- KERNEL PANIC (via Default Handler) ---\n");
        terminal_printf(" Unhandled Exception/Interrupt: %d (Error Code: 0x%x)\n", regs->int_no, regs->err_code);
        terminal_printf(" Occurred before scheduler context switch (current_process is NULL).\n");
        terminal_printf(" EIP: 0x%x CS: 0x%x EFLAGS: 0x%x\n", regs->eip, regs->cs, regs->eflags);
         if (regs->cs & 0x3) { // Check if CPU *thought* it was user mode (due to corruption)
             terminal_printf(" UserESP: 0x%x UserSS: 0x%x (Note: CPU was in user mode flag!)\n", regs->user_esp, regs->user_ss);
         } else {
             terminal_printf(" ESP: 0x%x SS: 0x%x\n", regs->esp_dummy, regs->ds); // Approx kernel stack? DS likely holds SS.
         }
         terminal_printf(" System Halted.\n");
        terminal_printf("-----------------------------------------\n");
        while(1) { __asm__ volatile ("cli; hlt"); } // Halt directly

    } else {
        // --- EXCEPTION/INTERRUPT AFTER SCHEDULER START ---
        // A process context exists, attempt graceful termination.
        terminal_printf("\n--- KERNEL EXCEPTION/INTERRUPT ---\n");
        terminal_printf(" Unhandled Exception/Interrupt: %d (Error Code: 0x%x)\n", regs->int_no, regs->err_code);
        terminal_printf(" EIP: 0x%x CS: 0x%x EFLAGS: 0x%x\n", regs->eip, regs->cs, regs->eflags);
        terminal_printf(" Terminating process (PID %d)\n", current_proc->pid);
        if (regs->cs & 0x3) { // Check if fault occurred in user mode
            terminal_printf(" UserESP: 0x%x UserSS: 0x%x\n", regs->user_esp, regs->user_ss);
        }
        terminal_printf("---------------------------------\n");

        // Terminate the current process. Use a specific exit code.
        uint32_t exit_code = 0xFF00 | regs->int_no; // Example: High byte FF, low byte is int number
        remove_current_task_with_code(exit_code);

        // remove_current_task_with_code should not return. Halt as a fallback.
        terminal_write("[PANIC] Error: remove_current_task_with_code returned!\n");
        while(1) { __asm__ volatile ("cli; hlt"); }
    }
}

 // Register a custom interrupt handler for vector 'num'.
 // *** Changed signature to match idt.h ***
 void register_int_handler(int num, void (*handler)(registers_t *regs), void* data) {
     if (num < 0 || num >= IDT_ENTRIES) return;
     interrupt_handlers[num].num = (uint16_t)num;
     // *** Assignment is now type-compatible ***
     interrupt_handlers[num].handler = handler;
     interrupt_handlers[num].data = data;
 }

 // Send End-Of-Interrupt (EOI) (implementation remains the same)
 static void send_eoi(int irq_number) {
     if (irq_number >= 8) { outb(PIC2_COMMAND, PIC_EOI); }
     outb(PIC1_COMMAND, PIC_EOI);
 }

 // C-level interrupt handler called by assembly stubs.
 // *** Changed signature to match idt.h ***
 void int_handler(registers_t *regs) {
     // Check if a custom handler is registered
     if (interrupt_handlers[regs->int_no].handler) {
         // *** Call handler with registers_t* ***
         interrupt_handlers[regs->int_no].handler(regs);
     } else {
         // No custom handler, use the default (which now terminates the process)
         default_int_handler(regs); // This should not return if it terminates the process
     }

     // Send EOI *only if* it was a hardware IRQ and the handler returned.
     if (regs->int_no >= 32 && regs->int_no <= 47) {
         send_eoi(regs->int_no - 32);
     }
 }

 // Initialize the IDT.
 void idt_init(void) {
     idtp.limit = (uint16_t)(sizeof(idt_entries) - 1);
     idtp.base  = (uint32_t)&idt_entries[0];

     // *** Use memset (needs <string.h>) ***
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
     idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E); // Invalid TSS
     idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E); // Segment Not Present
     idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E); // Stack-Segment Fault
     idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // General Protection Fault
     idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); // Page Fault
     idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E); // x87 FPU Error
     idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E); // Alignment Check
     idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E); // Machine Check
     idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E); // SIMD FP Exception
     // ISRs 20-31 reserved or architecture-specific

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
     idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E); // Primary ATA
     idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E); // Secondary ATA

     // Register Page Fault C Handler
     register_int_handler(14, page_fault_handler, NULL);

     // Load the IDT
     idt_load(&idtp);
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }