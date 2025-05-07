/**
 * @file idt.c
 * @brief Interrupt Descriptor Table (IDT) and PIC Management for UiAOS
 * @version 4.3 (Fixed PIC unmasking, style refinements)
 *
 * @details This file handles the crucial setup of the interrupt handling mechanism
 * on x86 systems. It includes:
 * - Remapping the legacy 8259 PICs to avoid conflicts with CPU exceptions.
 * - Defining and populating IDT gate entries for CPU exceptions (ISRs),
 * hardware interrupts (IRQs), and the system call interface.
 * - Loading the IDT Register (IDTR) using an assembly routine.
 * - Providing a mechanism to register C-level interrupt handlers.
 * - Implementing a common interrupt dispatcher (`isr_common_handler`)
 * called by assembly stubs.
 * - Sending End-of-Interrupt (EOI) signals to the PICs.
 * - Explicitly unmasking required hardware IRQs after initialization.
 */

//============================================================================
// Includes
//============================================================================

#include <idt.h>          // Primary header (includes types, constants, port_io, isr_frame)
#include <types.h>        // Base types
#include <string.h>       // memset
#include <serial.h>       // serial_write (for critical errors like double fault)
#include <assert.h>       // KERNEL_ASSERT, KERNEL_PANIC_HALT
#include <terminal.h>     // terminal_printf/write (for general logging)
#include <block_device.h> // Declaration for ata_primary_irq_handler

//============================================================================
// Definitions and Constants
//============================================================================

// --- IRQ Vectors (Derived from PIC start vectors defined in idt.h) ---
#define IRQ0_VECTOR  (PIC1_START_VECTOR + 0)  // PIT Timer
#define IRQ1_VECTOR  (PIC1_START_VECTOR + 1)  // Keyboard
#define IRQ2_VECTOR  (PIC1_START_VECTOR + 2)  // Cascade
// ... other IRQs ...
#define IRQ14_VECTOR (PIC2_START_VECTOR + 6)  // Primary ATA Hard Disk

// --- System Call Vector ---
#define SYSCALL_VECTOR 0x80

// --- IDT Gate Descriptor Flags ---
// Bits: P(resent) DPL(Privilege) S(torage=0) Type(Gate Type)
#define IDT_FLAG_INTERRUPT_GATE 0x8E // P=1, DPL=0, Type=0xE (32-bit Interrupt Gate, Interrupts Disabled)
#define IDT_FLAG_TRAP_GATE      0x8F // P=1, DPL=0, Type=0xF (32-bit Trap Gate, Interrupts Enabled)
#define IDT_FLAG_SYSCALL_GATE   0xEE // P=1, DPL=3, Type=0xE (Interrupt Gate, DPL 3 for user mode access)

// --- Kernel Code Segment Selector (from GDT) ---
// Assumes GDT is set up with Kernel Code at index 1 (selector 0x08)
#define KERNEL_CS_SELECTOR 0x08

// --- PIC Initialization Command Words (ICW) ---
#define ICW1_INIT        0x10        // Mask to start initialization sequence
#define ICW1_ICW4        0x01        // Mask indicating ICW4 is needed
#define ICW4_8086        0x01        // Mask setting 8086/88 (MCS-80/85) mode in ICW4

//============================================================================
// Module Static Data
//============================================================================

/**
 * @brief The Interrupt Descriptor Table (IDT) structure.
 * @details Contains 256 entries, aligned to 16 bytes for performance/compatibility.
 */
static struct idt_entry idt_entries[IDT_ENTRIES] __attribute__((aligned(16)));

/**
 * @brief Pointer structure for the LIDT instruction.
 * @details Holds the size (limit) and base address of the IDT.
 */
static struct idt_ptr idtp;

/**
 * @brief Array storing registration info for C-level interrupt handlers.
 * @details Indexed by the interrupt vector number.
 */
static interrupt_handler_info_t interrupt_c_handlers[IDT_ENTRIES];

//============================================================================
// External Assembly Routines
//============================================================================

// Assembly Interrupt Service Routine (ISR) stubs for exceptions 0-19
// These typically save all registers, call isr_common_handler, restore registers, and iret.
extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  /* ISR 9 reserved */ extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); /* ISR 15 reserved */
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
// Define others as needed

// Assembly Interrupt Request (IRQ) stubs for hardware interrupts 0-15
// Similar to ISR stubs, but might include EOI handling or call a different common handler.
extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

// Assembly System Call handler stub (vector 0x80)
// Handles transition from user mode, saving state, calling syscall dispatcher, restoring state, iret.
extern void syscall_handler_asm();

// Assembly routine to load the IDT register (lidt instruction)
// Parameter: Physical address of the idt_ptr structure.
extern void idt_flush(uintptr_t idt_ptr_addr);

//============================================================================
// PIC (8259 Programmable Interrupt Controller) Management
//============================================================================

/**
 * @brief Remaps the Master and Slave PICs to avoid conflicts with CPU exceptions.
 * @details Configures the PICs to map IRQs 0-7 to vectors PIC1_START_VECTOR to
 * PIC1_START_VECTOR+7, and IRQs 8-15 to vectors PIC2_START_VECTOR to
 * PIC2_START_VECTOR+7. Preserves the original interrupt masks.
 */
static void pic_remap(void) {
    // Save current masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // Start initialization sequence (in cascade mode)
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait(); // Wait for PIC to process command
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    // ICW2: Set vector offsets
    outb(PIC1_DATA, PIC1_START_VECTOR); // Master PIC vectors start at PIC1_START_VECTOR (e.g., 32)
    io_wait();
    outb(PIC2_DATA, PIC2_START_VECTOR); // Slave PIC vectors start at PIC2_START_VECTOR (e.g., 40)
    io_wait();

    // ICW3: Configure cascade connection (Master has Slave on IRQ2, Slave is cascade identity 2)
    outb(PIC1_DATA, 0x04); // Tell Master PIC that Slave is at IRQ2 (0000 0100)
    io_wait();
    outb(PIC2_DATA, 0x02); // Tell Slave PIC its cascade identity (0000 0010)
    io_wait();

    // ICW4: Set mode (8086/88 mode)
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    // Restore saved masks
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

    terminal_write("[IDT] PIC remapped.\n");
}

/**
 * @brief Sends an End-of-Interrupt (EOI) signal to the appropriate PIC(s).
 * @param vector The interrupt vector number (0-255) that was handled.
 * @details Must be called at the end of a hardware interrupt handler (IRQ)
 * to allow the PIC to signal further interrupts.
 */
static void pic_send_eoi(uint32_t vector) {
    // If the IRQ came from the Slave PIC, EOI must be sent to both
    if (vector >= PIC2_START_VECTOR && vector < PIC2_START_VECTOR + 8) {
        outb(PIC2_COMMAND, PIC_EOI); // Send EOI to Slave first
    }
    // Always send EOI to Master (Slave EOI cascaded through Master)
    outb(PIC1_COMMAND, PIC_EOI);
}

/**
 * @brief Unmasks specific IRQs required for basic system operation.
 * @details Reads the current masks, clears the bits for the required IRQs,
 * and writes the new masks back to the PICs. Includes I/O waits.
 */
static void pic_unmask_required_irqs(void) {
    terminal_write("[PIC] Unmasking required IRQs (Cascade IRQ2, ATA IRQ14)...\n");
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    terminal_printf("  [PIC] Current masks before unmask: Master=0x%02x, Slave=0x%02x\n", mask1, mask2);

    // Calculate new masks (clear bits to unmask/enable)
    // Bit 0 = IRQ 0, Bit 1 = IRQ 1, etc.
    uint8_t new_mask1 = mask1 & ~((1 << 0) | (1 << 1) | (1 << 2)); // Clear bits 0, 1, and 2
    uint8_t new_mask2 = mask2 & ~(1 << (14 - 8)); // Clear bit 6 for IRQ 14

    terminal_printf("  [PIC] Writing new masks: Master=0x%02x, Slave=0x%02x\n", new_mask1, new_mask2);

    // Write new masks
    outb(PIC1_DATA, new_mask1);
    io_wait(); // <<< FIX: Added I/O wait after write
    outb(PIC2_DATA, new_mask2);
    io_wait(); // <<< FIX: Added I/O wait after write

    // Optional: Read back and log for verification
    uint8_t final_mask1 = inb(PIC1_DATA);
    uint8_t final_mask2 = inb(PIC2_DATA);
    terminal_printf("   [PIC] Read back masks: Master=0x%02x, Slave=0x%02x\n", final_mask1, final_mask2);;

    if ((final_mask1 & 0x02) != 0) { // Check if bit 1 is still set
        KERNEL_PANIC_HALT("Failed to unmask IRQ1 on Master PIC!");
    }

    // Optional assertion (if needed, could be too strict for some HW/VMs)
    // KERNEL_ASSERT(final_mask1 == new_mask1 && final_mask2 == new_mask2, "PIC mask write failed verification");
}


//============================================================================
// IDT Gate Setup
//============================================================================

/**
 * @brief Configures a single gate descriptor in the IDT.
 * @param num       The interrupt vector number (0-255) for this gate.
 * @param base      The 32-bit virtual address of the interrupt handler function.
 * @param selector  The Code Segment selector (usually KERNEL_CS_SELECTOR).
 * @param flags     The gate type and flags (e.g., IDT_FLAG_INTERRUPT_GATE).
 */
static void idt_set_gate_internal(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);         // Lower 16 bits of handler address
    idt_entries[num].sel       = selector;                          // Code segment selector
    idt_entries[num].null      = 0;                                 // Always zero
    idt_entries[num].flags     = flags;                             // Type and attributes (P, DPL, Type)
    idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF); // Upper 16 bits of handler address
}

//============================================================================
// C Handler Registration and Dispatch
//============================================================================

/**
 * @brief Registers a C function to handle a specific interrupt vector.
 * @param vector    The interrupt vector number (0-255) to associate the handler with.
 * @param handler   Pointer to the C handler function (must match `int_handler_t` signature).
 * @param data      Optional pointer to pass context data to the handler (currently unused).
 * @details Ensures the vector number is valid and a handler isn't already registered.
 */
void register_int_handler(int vector, int_handler_t handler, void* data) {
    KERNEL_ASSERT(vector >= 0 && vector < IDT_ENTRIES, "register_int_handler: Invalid vector number");
    KERNEL_ASSERT(handler != NULL, "register_int_handler: NULL handler registered");
    // Allow re-registration if needed, but assert for now to catch potential issues
    KERNEL_ASSERT(interrupt_c_handlers[vector].handler == NULL, "register_int_handler: Handler already registered for vector");

    interrupt_c_handlers[vector].handler = handler;
    interrupt_c_handlers[vector].data    = data; // Store context data (not used by common handler yet)
}

/**
 * @brief Default C handler for unhandled interrupts/exceptions.
 * @param frame Pointer to the interrupt stack frame containing CPU state.
 * @details Prints detailed debug information about the CPU state at the time
 * of the interrupt/exception and halts the system. This is crucial
 * for diagnosing unexpected interrupts or faults.
 */
void default_isr_handler(isr_frame_t* frame) {
    // Use serial port for critical errors like double faults or early panics
    serial_write("\n*** Unhandled Interrupt/Exception ***\n");
    serial_write(" -> Check terminal output for details.\n");

    // Print detailed info to the main terminal
    terminal_printf("\n*** KERNEL PANIC: Unhandled Interrupt/Exception ***\n");
    KERNEL_ASSERT(frame != NULL, "default_isr_handler received NULL frame!");

    terminal_printf(" Vector:  %lu (0x%lx)\n", (unsigned long)frame->int_no, (unsigned long)frame->int_no);
    terminal_printf(" ErrCode: 0x%lx\n", (unsigned long)frame->err_code);
    terminal_printf(" EIP:     0x%08lx\n", (unsigned long)frame->eip);
    terminal_printf(" CS:      0x%lx\n", (unsigned long)frame->cs);
    terminal_printf(" EFLAGS:  0x%lx\n", (unsigned long)frame->eflags);

    // Check if the CPU pushed user ESP/SS (happens on privilege change)
    // Check Ring Privilege Level (RPL) in CS selector (bits 0-1)
    if ((frame->cs & 0x3) != 0) { // If RPL != 0, it came from user mode
        terminal_printf(" UserESP: 0x%p\n", (void*)frame->useresp); // %p for address
        terminal_printf(" UserSS:  0x%lx\n", (unsigned long)frame->ss);
    }

    // Special handling for Page Fault (#PF) - Decode error code and CR2
    if (frame->int_no == 14) { // Page Fault vector number
        uintptr_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2)); // Read faulting address from CR2
        terminal_printf(" Fault Address (CR2): 0x%p\n", (void*)cr2);
        terminal_printf(" PF Error Decode: [%s %s %s %s %s]\n",
                      (frame->err_code & 0x1) ? "Present" : "Not-Present",
                      (frame->err_code & 0x2) ? "Write" : "Read",
                      (frame->err_code & 0x4) ? "User" : "Supervisor",
                      (frame->err_code & 0x8) ? "Reserved-Bit-Set" : "Reserved-OK",
                      (frame->err_code & 0x10) ? "Instruction-Fetch" : "Data-Access");
    }

    // Print general-purpose registers
    terminal_printf(" --- Registers ---\n");
    terminal_printf(" EAX=0x%lx EBX=0x%lx ECX=0x%lx EDX=0x%lx\n",
                   (unsigned long)frame->eax, (unsigned long)frame->ebx,
                   (unsigned long)frame->ecx, (unsigned long)frame->edx);
    terminal_printf(" ESI=0x%lx EDI=0x%lx EBP=0x%p\n", // Use %p for EBP (stack frame pointer)
                   (unsigned long)frame->esi, (unsigned long)frame->edi, (void*)frame->ebp);
    terminal_printf(" DS=0x%lx ES=0x%lx FS=0x%lx GS=0x%lx\n",
                   (unsigned long)frame->ds, (unsigned long)frame->es,
                   (unsigned long)frame->fs, (unsigned long)frame->gs);
    terminal_printf(" -----------------------------------\n");
    terminal_write(" System Halted.\n");

    // Halt indefinitely
    while (1) { asm volatile ("cli; hlt"); }
}

/**
 * @brief Common C interrupt dispatcher called by assembly stubs.
 * @param frame Pointer to the interrupt stack frame.
 * @details Determines the interrupt vector number, looks up the registered
 * C handler, and calls it. If no handler is registered, calls the
 * default handler. Sends EOI to the PIC for hardware interrupts.
 */
void isr_common_handler(isr_frame_t* frame) {
    serial_write("[IDT] Enter isr_common_handler\n");

    // Basic validation of the frame pointer itself
    if (!frame) { KERNEL_PANIC_HALT("isr_common_handler received NULL frame!"); }

    uint32_t vector = frame->int_no;

    // Validate vector range
    if (vector >= IDT_ENTRIES) {
        serial_write("[IDT ERROR] Invalid vector number received by C dispatcher!\n");
        // Attempt to log details via default handler before panicking
        default_isr_handler(frame);
        // If default_isr_handler returns (it shouldn't), panic explicitly.
        KERNEL_PANIC_HALT("Invalid vector number processed by default handler!");
    }

    interrupt_handler_info_t* entry = &interrupt_c_handlers[vector];

    // Call the registered handler if one exists
    if (entry->handler != NULL) {
        entry->handler(frame); // Pass the frame pointer to the specific handler
    } else {
        // Handle specific critical unhandled exceptions if needed
        if (vector == 8) { // Double Fault - Very serious!
            serial_write("\n*** DOUBLE FAULT *** System Halted.\n");
            // Attempt to print diagnostics via default handler
            default_isr_handler(frame);
             // Explicit halt loop for Double Fault, default_isr_handler might fail
            while(1) asm volatile("cli; hlt");
        }
        // Use the default handler for all other unassigned interrupts/exceptions
        default_isr_handler(frame);
    }

    // Send End-of-Interrupt (EOI) signal *after* the handler has run
    // ONLY for hardware interrupts originating from the PICs (vectors 32-47 typically).
    if (vector >= PIC1_START_VECTOR && vector < PIC2_START_VECTOR + 8) {
       pic_send_eoi(vector);
    }
}


//============================================================================
// Public Initialization Function
//============================================================================

/**
 * @brief Initializes the entire IDT subsystem.
 * @details Clears internal structures, sets up the IDT pointer, remaps the PICs,
 * populates the IDT with gates pointing to assembly stubs for exceptions,
 * IRQs, and syscalls, registers essential C handlers (like the ATA IRQ),
 * loads the IDT register (IDTR) via `idt_flush`, and unmasks required IRQs.
 */
void idt_init(void) {
    terminal_write("[IDT] Initializing IDT and PIC...\n");

    // Clear internal data structures
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));

    // Setup the IDT pointer structure for LIDT
    idtp.limit = sizeof(idt_entries) - 1;
    idtp.base  = (uintptr_t)&idt_entries[0]; // Base address of the IDT array

    // Remap PICs to avoid conflicts with CPU exceptions
    pic_remap();

    // --- Populate IDT Gates ---
    terminal_write("[IDT] Registering Exception handlers (ISRs 0-19)...\n");
    idt_set_gate_internal(0, (uint32_t)isr0, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(1, (uint32_t)isr1, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(2, (uint32_t)isr2, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(3, (uint32_t)isr3, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(4, (uint32_t)isr4, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(5, (uint32_t)isr5, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(6, (uint32_t)isr6, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(7, (uint32_t)isr7, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Device Not Available
    idt_set_gate_internal(8, (uint32_t)isr8, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Double Fault
    // ISR 9 is reserved
    idt_set_gate_internal(10, (uint32_t)isr10, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Invalid TSS
    idt_set_gate_internal(11, (uint32_t)isr11, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Segment Not Present
    idt_set_gate_internal(12, (uint32_t)isr12, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Stack Segment Fault
    idt_set_gate_internal(13, (uint32_t)isr13, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // General Protection Fault
    idt_set_gate_internal(14, (uint32_t)isr14, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Page Fault
    // ISR 15 is reserved
    idt_set_gate_internal(16, (uint32_t)isr16, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // x87 Floating Point Exception
    idt_set_gate_internal(17, (uint32_t)isr17, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Alignment Check
    idt_set_gate_internal(18, (uint32_t)isr18, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Machine Check
    idt_set_gate_internal(19, (uint32_t)isr19, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // SIMD Floating Point Exception
    // Define others as needed (Vectors 20-31 are reserved or architecture specific)

    terminal_write("[IDT] Registering Hardware Interrupt handlers (IRQs -> Vectors 32-47)...\n");
    idt_set_gate_internal(IRQ0_VECTOR + 0, (uint32_t)irq0, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // PIT
    idt_set_gate_internal(IRQ0_VECTOR + 1, (uint32_t)irq1, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Keyboard
    idt_set_gate_internal(IRQ0_VECTOR + 2, (uint32_t)irq2, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Cascade
    idt_set_gate_internal(IRQ0_VECTOR + 3, (uint32_t)irq3, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // COM2
    idt_set_gate_internal(IRQ0_VECTOR + 4, (uint32_t)irq4, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // COM1
    idt_set_gate_internal(IRQ0_VECTOR + 5, (uint32_t)irq5, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // LPT2
    idt_set_gate_internal(IRQ0_VECTOR + 6, (uint32_t)irq6, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Floppy Disk
    idt_set_gate_internal(IRQ0_VECTOR + 7, (uint32_t)irq7, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // LPT1 / Spurious
    idt_set_gate_internal(IRQ0_VECTOR + 8, (uint32_t)irq8, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // RTC
    idt_set_gate_internal(IRQ0_VECTOR + 9, (uint32_t)irq9, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Free/Network
    idt_set_gate_internal(IRQ0_VECTOR + 10, (uint32_t)irq10, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Free/SCSI/Network
    idt_set_gate_internal(IRQ0_VECTOR + 11, (uint32_t)irq11, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Free/SCSI/Network
    idt_set_gate_internal(IRQ0_VECTOR + 12, (uint32_t)irq12, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // PS/2 Mouse
    idt_set_gate_internal(IRQ0_VECTOR + 13, (uint32_t)irq13, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // FPU/Coprocessor
    idt_set_gate_internal(IRQ0_VECTOR + 14, (uint32_t)irq14, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Primary ATA HD
    idt_set_gate_internal(IRQ0_VECTOR + 15, (uint32_t)irq15, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE); // Secondary ATA HD

    // --- Register System Call Gate ---
    terminal_write("[IDT] Registering System Call handler...\n");
    // Use DPL=3 to allow user mode code to trigger this interrupt via INT 0x80
    idt_set_gate_internal(SYSCALL_VECTOR, (uint32_t)syscall_handler_asm, KERNEL_CS_SELECTOR, IDT_FLAG_SYSCALL_GATE);
    terminal_printf("[IDT] Registered syscall handler at vector 0x%x\n", SYSCALL_VECTOR);

    // --- Register Specific C Handlers for Drivers ---
    terminal_write("[IDT] Registering ATA Primary IRQ handler (Vector 46).\n");
    KERNEL_ASSERT(ata_primary_irq_handler != NULL, "ata_primary_irq_handler is NULL");
    register_int_handler(IRQ14_VECTOR, ata_primary_irq_handler, NULL);
    // Register other handlers (PIT, Keyboard, etc.) here or in their respective init functions

    // --- Load the IDT Register (IDTR) ---
    // Use %hu for uint16_t (limit), %p for pointer (base)
    terminal_printf("[IDT] Loading IDTR: Limit=0x%hX Base=%#010lx (Virt Addr)\n",
                    idtp.limit, (void*)idtp.base);
    idt_flush((uintptr_t)&idtp); // Pass the virtual address of the idtp structure

    terminal_write("[IDT] IDT initialized and loaded.\n");

    // --- Unmask necessary IRQs on the PIC ---
    // This allows enabled interrupts (like PIT, keyboard, ATA) to reach the CPU.
    pic_unmask_required_irqs();

    terminal_write("[IDT] Setup complete.\n");
}