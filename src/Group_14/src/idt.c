/**
 * idt.c – Updated IDT implementation for UiAOS
 * Uses the new isr_frame_t structure and registers handlers individually.
 * Includes registration for the real ATA primary IRQ handler.
 */

 #include <idt.h>          // Primary header (includes idt_entry, idt_ptr)
 #include <isr_frame.h>    // Include the new frame definition
 #include <terminal.h>
 #include <port_io.h>      // For outb, inb, io_wait
 #include <types.h>        // For uintN_t types
 #include <string.h>       // For memset
 #include <serial.h>       // For serial debugging (optional)
 #include <assert.h>       // For KERNEL_PANIC_HALT
 #include <block_device.h> // <<< ADDED: Include if ata_primary_irq_handler declaration is here
 
 //--------------------------------------------------------------------------------------------------
 //  Internal data
 //--------------------------------------------------------------------------------------------------
 
 static struct idt_entry idt_entries[IDT_ENTRIES];
 static struct idt_ptr   idtp;
 
 // Structure to hold registered C handler info
 typedef struct interrupt_handler_c_info {
     int           num;      // Interrupt number
     void          (*handler)(isr_frame_t* frame); // C Handler function pointer using new frame type
     void* data;     // Optional data pointer for the handler
 } interrupt_handler_c_info_t;
 
 static interrupt_handler_c_info_t interrupt_c_handlers[IDT_ENTRIES]; // Array to store registered C handlers
 
 //--------------------------------------------------------------------------------------------------
 //  External ISR / IRQ stubs from corrected assembly files
 //--------------------------------------------------------------------------------------------------
 
 // Exceptions (0-19) - Ensure these match your isr_stubs.asm
 extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
 extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
 extern void isr8();  /* ISR 9 maybe missing? */ extern void isr10(); extern void isr11();
 extern void isr12(); extern void isr13(); extern void isr14(); /* ISR 15 maybe missing? */
 extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
 // Add externs for 20-31 if you have stubs for them
 
 // IRQs (0-15 -> Vectors 32-47) - Ensure these match your irq_stubs.asm
 extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
 extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
 extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
 extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();
 
 // Syscall handler
 extern void syscall_handler_asm(); // From syscall.asm
 
 // ATA handler (Defined in block_device.c or similar)
 // Ensure its declaration is available, e.g., via block_device.h
 extern void ata_primary_irq_handler(isr_frame_t* frame);
 // extern void ata_secondary_irq_handler(isr_frame_t* frame); // If implemented
 
 //--------------------------------------------------------------------------------------------------
 //  PIC Remapping (Unchanged)
 //--------------------------------------------------------------------------------------------------
 static inline void pic_remap(void)
 {
     uint8_t m1 = inb(PIC1_DATA); // Save masks
     uint8_t m2 = inb(PIC2_DATA);
 
     outb(PIC1_COMMAND, 0x11); // Start initialization sequence (ICW1)
     io_wait();
     outb(PIC2_COMMAND, 0x11);
     io_wait();
     outb(PIC1_DATA, 0x20); // ICW2: Master PIC vector offset (IRQ 0-7 -> INT 32-39)
     io_wait();
     outb(PIC2_DATA, 0x28); // ICW2: Slave PIC vector offset (IRQ 8-15 -> INT 40-47)
     io_wait();
     outb(PIC1_DATA, 0x04); // ICW3: Tell Master PIC slave is at IRQ2 (0000 0100)
     io_wait();
     outb(PIC2_DATA, 0x02); // ICW3: Tell Slave PIC its cascade identity (0000 0010)
     io_wait();
     outb(PIC1_DATA, 0x01); // ICW4: 8086 mode
     io_wait();
     outb(PIC2_DATA, 0x01); // ICW4: 8086 mode
     io_wait();
     outb(PIC1_DATA, m1);   // Restore saved masks
     outb(PIC2_DATA, m2);
 }
 
 //--------------------------------------------------------------------------------------------------
 //  IDT Gate Setup (Unchanged)
 //--------------------------------------------------------------------------------------------------
 static inline void idt_set_gate(uint8_t num, uint32_t base)
 {
     idt_entries[num].base_low  = base & 0xFFFF;
     idt_entries[num].base_high = (base >> 16) & 0xFFFF;
     idt_entries[num].sel       = 0x08;     // Kernel Code Segment Selector
     idt_entries[num].null      = 0;        // Always zero bits
     idt_entries[num].flags     = 0x8E;     // Present=1, DPL=0, Type=0xE (32-bit Interrupt Gate)
 }
 
 static inline void idt_set_syscall_gate(uint8_t num, uint32_t base)
 {
     idt_entries[num].base_low  = base & 0xFFFF;
     idt_entries[num].base_high = (base >> 16) & 0xFFFF;
     idt_entries[num].sel       = 0x08;     // Kernel Code Segment Selector
     idt_entries[num].null      = 0;
     idt_entries[num].flags     = 0xEE;     // Present=1, DPL=3, Type=0xE (32-bit Trap Gate) -> Should likely be 0xEF for Trap Gate allowing user calls
                                             // Note: Check Trap vs Interrupt gate differences if issues arise. 0xEE is Interrupt Gate with DPL=3. 0xEF is Trap Gate DPL=3.
 }
 
 
 // Load IDT Register (Assembly function, e.g., in idt_flush.asm)
 extern void idt_flush(uint32_t idt_ptr_addr);
 
 //--------------------------------------------------------------------------------------------------
 //  C Handler Registration and Dispatch (Unchanged)
 //--------------------------------------------------------------------------------------------------
 
 void register_int_handler(int num, void (*handler)(isr_frame_t*), void* data)
 {
     if (num >= 0 && num < IDT_ENTRIES)
     {
         interrupt_c_handlers[num].num     = num;
         interrupt_c_handlers[num].handler = handler;
         interrupt_c_handlers[num].data    = data;
     } else {
         terminal_printf("[IDT WARN] Attempt to register handler for invalid vector %d\n", num);
     }
 }
 
 static void send_eoi(uint32_t int_no)
 {
     if (int_no >= 32 && int_no <= 47) {
         if (int_no >= 40) {
             outb(PIC2_COMMAND, PIC_EOI);
         }
         outb(PIC1_COMMAND, PIC_EOI);
     }
 }
 
 void default_isr_handler(isr_frame_t* frame)
 {
     // (Default handler implementation unchanged from previous version)
     terminal_printf("\n*** Unhandled Interrupt/Exception ***\n");
     terminal_printf(" Vector: %lu (0x%lx)\n", (unsigned long)frame->int_no, (unsigned long)frame->int_no);
     terminal_printf(" ErrCode: %#lx\n", (unsigned long)frame->err_code);
     terminal_printf(" EIP: %p CS: %#lx EFLAGS: %#lx\n",
                     (void*)frame->eip, (unsigned long)frame->cs, (unsigned long)frame->eflags);
     if ((frame->cs & 0x3) == 3) {
         terminal_printf(" UserESP: %p SS: %#lx\n", (void*)frame->useresp, (unsigned long)frame->ss);
     }
     if (frame->int_no == 14) { // Page Fault
         uintptr_t cr2;
         asm volatile("mov %%cr2, %0" : "=r"(cr2));
         terminal_printf(" Fault Address (CR2): %p\n", (void*)cr2);
         terminal_printf(" PF Error Code: [%s %s %s %s %s]\n",
                         (frame->err_code & 0x1) ? "P" : "NP", (frame->err_code & 0x2) ? "W" : "R",
                         (frame->err_code & 0x4) ? "User" : "Super", (frame->err_code & 0x8) ? "Res" : "-",
                         (frame->err_code & 0x10) ? "IFetch" : "Data");
     }
     terminal_printf("-----------------------------------\n");
     terminal_printf(" EAX=%#lx EBX=%#lx ECX=%#lx EDX=%#lx\n", frame->eax, frame->ebx, frame->ecx, frame->edx);
     terminal_printf(" ESI=%#lx EDI=%#lx EBP=%#lx\n", frame->esi, frame->edi, frame->ebp);
     terminal_printf(" DS=%#lx ES=%#lx FS=%#lx GS=%#lx\n", frame->ds, frame->es, frame->fs, frame->gs);
     terminal_printf("-----------------------------------\n");
     terminal_write(" System Halted.\n");
     // Use loop for serial write as fixed previously
     const char* halt_msg = "\nSystem Halted due to unhandled interrupt.\n";
     for (int i = 0; halt_msg[i] != '\0'; ++i) {
         serial_write(halt_msg[i]); // Assuming serial_write(char) exists
     }
     while (1) __asm__ volatile ("cli; hlt");
 }
 
 // Common C handler called by assembly stubs (`common_interrupt_stub`)
 void isr_common_handler(isr_frame_t* frame) {
     if (frame->int_no < IDT_ENTRIES && interrupt_c_handlers[frame->int_no].handler != NULL) {
         interrupt_c_handlers[frame->int_no].handler(frame);
     } else {
         if (frame->int_no == 8) { // Double Fault
              terminal_printf("\n*** DOUBLE FAULT ***\n");
              terminal_printf(" ErrCode: %#lx\n", (unsigned long)frame->err_code);
              terminal_printf(" EIP: %p CS: %#lx EFLAGS: %#lx\n",
                              (void*)frame->eip, (unsigned long)frame->cs, (unsigned long)frame->eflags);
              const char* df_msg = "\n*** DOUBLE FAULT *** System Halted.\n";
              for (int i = 0; df_msg[i] != '\0'; ++i) serial_write(df_msg[i]);
              while(1) asm volatile("cli; hlt");
         }
         // Special handling for Page Fault if not using exception table approach fully
         // For now, let it fall through to default handler if no C handler registered
         // else if (frame->int_no == 14) { ... }
 
         default_isr_handler(frame); // Handle other unassigned interrupts
     }
     send_eoi(frame->int_no); // Send EOI AFTER handler runs
 }
 
 
 //--------------------------------------------------------------------------------------------------
 //  Public init function
 //--------------------------------------------------------------------------------------------------
 void idt_init(void)
 {
     terminal_write("[IDT] Initializing IDT and PIC...\n");
     memset(idt_entries, 0, sizeof(idt_entries));
     memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));
 
     idtp.limit = sizeof(idt_entries) - 1;
     idtp.base  = (uint32_t)idt_entries;
 
     pic_remap();
     terminal_write("[IDT] PIC remapped.\n");
 
     // Register assembly stubs for exceptions (0-19)
     terminal_write("[IDT] Registering Exception handlers (ISRs 0-19)...\n");
     idt_set_gate(0, (uint32_t)isr0);   idt_set_gate(1, (uint32_t)isr1);
     idt_set_gate(2, (uint32_t)isr2);   idt_set_gate(3, (uint32_t)isr3);
     idt_set_gate(4, (uint32_t)isr4);   idt_set_gate(5, (uint32_t)isr5);
     idt_set_gate(6, (uint32_t)isr6);   idt_set_gate(7, (uint32_t)isr7);
     idt_set_gate(8, (uint32_t)isr8);
     // idt_set_gate(9, (uint32_t)isr9); // If isr9 exists
     idt_set_gate(10, (uint32_t)isr10); idt_set_gate(11, (uint32_t)isr11);
     idt_set_gate(12, (uint32_t)isr12); idt_set_gate(13, (uint32_t)isr13);
     idt_set_gate(14, (uint32_t)isr14); // Assuming isr14 is defined (likely in isr_pf.asm now)
     // idt_set_gate(15, (uint32_t)isr15); // If isr15 exists
     idt_set_gate(16, (uint32_t)isr16); idt_set_gate(17, (uint32_t)isr17);
     idt_set_gate(18, (uint32_t)isr18); idt_set_gate(19, (uint32_t)isr19);
     // Add gates for 20-31 if needed
 
     // Register assembly stubs for IRQs (32-47)
     terminal_write("[IDT] Registering Hardware Interrupt handlers (IRQs 0-15 -> Vectors 32-47)...\n");
     idt_set_gate(32, (uint32_t)irq0);  idt_set_gate(33, (uint32_t)irq1);
     idt_set_gate(34, (uint32_t)irq2);  idt_set_gate(35, (uint32_t)irq3);
     idt_set_gate(36, (uint32_t)irq4);  idt_set_gate(37, (uint32_t)irq5);
     idt_set_gate(38, (uint32_t)irq6);  idt_set_gate(39, (uint32_t)irq7);
     idt_set_gate(40, (uint32_t)irq8);  idt_set_gate(41, (uint32_t)irq9);
     idt_set_gate(42, (uint32_t)irq10); idt_set_gate(43, (uint32_t)irq11);
     idt_set_gate(44, (uint32_t)irq12); idt_set_gate(45, (uint32_t)irq13);
     idt_set_gate(46, (uint32_t)irq14); idt_set_gate(47, (uint32_t)irq15);
 
     // --- Register C Handlers ---
     // Example: Register PIT (IRQ0->Vec32), Keyboard (IRQ1->Vec33) handlers if defined
     // extern void pit_irq_handler(isr_frame_t*); // Make sure declared
     // register_int_handler(32, pit_irq_handler, NULL);
     // extern void keyboard_handler(isr_frame_t*); // Make sure declared
     // register_int_handler(33, keyboard_handler, NULL);
 
     // Register the *real* C handler for Vector 46 (IRQ 14 - Primary ATA)
     terminal_write("[IDT] Registering ATA Primary IRQ handler (Vector 46).\n");
     register_int_handler(46, ata_primary_irq_handler, NULL); // <-- Use the real handler
 
     // Register the C handler for Page Faults (Vector 14) if isr14 just jumps to common stub
     // If isr_pf.asm calls page_fault_handler directly, this isn't needed.
     // extern void page_fault_handler(isr_frame_t* frame);
     // register_int_handler(14, page_fault_handler, NULL);
 
     // Register system call handler (INT 0x80) using a trap gate
     idt_set_syscall_gate(0x80, (uint32_t)syscall_handler_asm);
     terminal_printf("[IDT] Registered syscall handler at interrupt 0x80\n");
 
     // Load the IDT register
     terminal_printf("[IDT DEBUG] Loading IDTR: Limit=0x%hx Base=%p\n",
                     idtp.limit, (void*)idtp.base);
     idt_flush((uint32_t)&idtp); // Pass the address of the idtp struct
 
     terminal_write("[IDT] IDT initialized and loaded.\n");
 }