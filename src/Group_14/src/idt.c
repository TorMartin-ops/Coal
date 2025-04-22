/**
 * idt.c - Interrupt Descriptor Table (IDT) Management for UiAOS
 *
 * Version: 4.2 (Fixed type mismatch for isr_frame_t, added EOI call back)
 *
 * Description:
 * This file handles the setup of the IDT, including:
 * - Remapping the legacy PIC (Programmable Interrupt Controller).
 * - Defining IDT gate entries for exceptions (ISRs), hardware interrupts (IRQs),
 * and system calls.
 * - Loading the IDT Register (IDTR).
 * - Registering C-level interrupt handlers.
 * - Providing a common interrupt dispatcher (`isr_common_handler`).
 * - Explicitly unmasking required IRQs (like ATA IRQ 14) after setup.
 */

// === Core Headers ===
#include <idt.h>          // Primary header (includes types, constants, port_io, isr_frame)
#include <types.h>        // Base types (already included via idt.h)
#include <string.h>       // memset
#include <serial.h>       // serial_write
#include <assert.h>       // KERNEL_ASSERT, KERNEL_PANIC_HALT

// === Subsystems/Drivers ===
#include <terminal.h>     // terminal_printf/write for general logging
#include <block_device.h> // Declaration for ata_primary_irq_handler

//----------------------------------------------------------------------------
// Constants and Macros
//----------------------------------------------------------------------------

// Specific IRQ Vectors (Defined in idt.h)
#define IRQ0_VECTOR  (PIC1_START_VECTOR + 0)
#define IRQ1_VECTOR  (PIC1_START_VECTOR + 1)
#define IRQ2_VECTOR  (PIC1_START_VECTOR + 2)
#define IRQ14_VECTOR (PIC2_START_VECTOR + 6)

// System Call Vector (Defined in idt.h)
#define SYSCALL_VECTOR 0x80

// IDT Gate Flags
#define IDT_FLAG_INTERRUPT_GATE 0x8E // P=1, DPL=0, Type=0xE (32-bit Interrupt Gate)
#define IDT_FLAG_TRAP_GATE      0x8F // P=1, DPL=0, Type=0xF (32-bit Trap Gate)
#define IDT_FLAG_SYSCALL_GATE   0xEE // P=1, DPL=3, Type=0xE (Interrupt Gate, DPL 3)

// Kernel Code Segment Selector (from GDT)
#define KERNEL_CS_SELECTOR 0x08

// --- Define missing PIC constants --- (Also defined in idt.h, keep for clarity?)
#define ICW1_INIT    0x10        // Required to start initialization sequence
#define ICW1_ICW4    0x01        // Indicates ICW4 will be present
#define ICW4_8086    0x01        // Sets 8086/88 (MCS-80/85) mode

//----------------------------------------------------------------------------
// Module Static Data
//----------------------------------------------------------------------------

// The Interrupt Descriptor Table itself (256 entries)
static struct idt_entry idt_entries[IDT_ENTRIES] __attribute__((aligned(16)));

// Pointer structure for the LIDT instruction
static struct idt_ptr idtp;

// Array to store registered C handler function pointers
// Uses struct interrupt_handler_info_t defined in idt.h
static interrupt_handler_info_t interrupt_c_handlers[IDT_ENTRIES];

//----------------------------------------------------------------------------
// External Assembly Routines (ISR/IRQ/Syscall Stubs, IDT Flush)
//----------------------------------------------------------------------------

// Assembly Interrupt Service Routine (ISR) stubs for exceptions 0-19
extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  /* ISR 9 reserved */ extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); /* ISR 15 reserved */
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();

// Assembly Interrupt Request (IRQ) stubs for hardware interrupts 0-15
extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

// Assembly System Call handler stub
extern void syscall_handler_asm();

// Assembly routine to load the IDT register (lidt)
extern void idt_flush(uintptr_t idt_ptr_addr);

//----------------------------------------------------------------------------
// PIC (Programmable Interrupt Controller) Management
//----------------------------------------------------------------------------

/**
 * @brief Remaps the PICs to avoid conflicts with CPU exceptions.
 */
static void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC1_DATA, PIC1_START_VECTOR);
    io_wait();
    outb(PIC2_DATA, PIC2_START_VECTOR);
    io_wait();
    outb(PIC1_DATA, 4);
    io_wait();
    outb(PIC2_DATA, 2);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
    terminal_write("[IDT] PIC remapped.\n");
}

/**
 * @brief Sends End-of-Interrupt (EOI) signal to PIC(s).
 * @param vector The interrupt vector number (0-255).
 */
static void pic_send_eoi(uint32_t vector) {
    if (vector >= PIC1_START_VECTOR && vector < PIC1_START_VECTOR + 8) {
        outb(PIC1_COMMAND, PIC_EOI);
    } else if (vector >= PIC2_START_VECTOR && vector < PIC2_START_VECTOR + 8) {
        outb(PIC2_COMMAND, PIC_EOI);
        outb(PIC1_COMMAND, PIC_EOI); // Also EOI master for slave IRQs
    }
}

/**
 * @brief Unmasks specific IRQs required for basic operation after remapping.
 */
static void pic_unmask_required_irqs(void) {
    terminal_write("[PIC] Unmasking required IRQs (Cascade IRQ2, ATA IRQ14)...\n");
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    terminal_printf("  [PIC] Current masks before unmask: Master=0x%02x, Slave=0x%02x\n", mask1, mask2);
    mask1 &= ~(1 << 2); // Unmask IRQ 2 (Cascade)
    mask2 &= ~(1 << 6); // Unmask IRQ 14 (Slave IRQ 6)
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);
    terminal_printf("  [PIC] New masks after unmask: Master=0x%02x, Slave=0x%02x\n", mask1, mask2);
}


//----------------------------------------------------------------------------
// IDT Gate Setup
//----------------------------------------------------------------------------

/**
 * @brief Sets an entry in the IDT.
 */
static void idt_set_gate_internal(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    idt_entries[num].base_low  = base & 0xFFFF;
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
    idt_entries[num].sel       = selector;
    idt_entries[num].null      = 0;
    idt_entries[num].flags     = flags;
}

//----------------------------------------------------------------------------
// C Handler Registration and Dispatch
//----------------------------------------------------------------------------

/**
 * @brief Registers a C function to handle a specific interrupt vector.
 */
 // <<< Uses int_handler_t (defined with isr_frame_t*) from idt.h >>>
void register_int_handler(int vector, int_handler_t handler, void* data) {
    KERNEL_ASSERT(vector >= 0 && vector < IDT_ENTRIES, "Invalid vector number");
    KERNEL_ASSERT(handler != NULL, "NULL handler registered");
    KERNEL_ASSERT(interrupt_c_handlers[vector].handler == NULL, "Handler already registered");

    interrupt_c_handlers[vector].handler = handler;
    interrupt_c_handlers[vector].data    = data;
}

/**
 * @brief Default C handler for unhandled interrupts/exceptions. Prints debug info and halts.
 */
 // <<< FIXED: Use isr_frame_t* consistently >>>
void default_isr_handler(isr_frame_t* frame) {
    serial_write("\n*** Unhandled Interrupt/Exception ***\n");
    serial_write(" -> Check terminal output for details.\n");

    terminal_printf("\n*** Unhandled Interrupt/Exception ***\n");
    terminal_printf(" Vector: %lu (0x%lx)\n", (unsigned long)frame->int_no, (unsigned long)frame->int_no);
    terminal_printf(" ErrCode: 0x%lx\n", (unsigned long)frame->err_code);
    terminal_printf(" EIP: 0x%lx  CS: 0x%lx  EFLAGS: 0x%lx\n",
                   (unsigned long)frame->eip, (unsigned long)frame->cs, (unsigned long)frame->eflags);
    if ((frame->cs & 0x3) == 3) {
        terminal_printf(" UserESP: 0x%lx  SS: 0x%lx\n", (unsigned long)frame->useresp, (unsigned long)frame->ss);
    }
    if (frame->int_no == 14) {
        uintptr_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_printf(" Fault Address (CR2): 0x%lx\n", (unsigned long)cr2);
        terminal_printf(" PF Error Code: [%s %s %s %s %s]\n",
                      (frame->err_code & 0x1) ? "P" : "NP", (frame->err_code & 0x2) ? "W" : "R",
                      (frame->err_code & 0x4) ? "User" : "Super", (frame->err_code & 0x8) ? "Res" : "-",
                      (frame->err_code & 0x10) ? "IFetch" : "Data");
    }
    terminal_printf("-----------------------------------\n");
    terminal_printf(" EAX=0x%lx EBX=0x%lx ECX=0x%lx EDX=0x%lx\n", (unsigned long)frame->eax, (unsigned long)frame->ebx, (unsigned long)frame->ecx, (unsigned long)frame->edx);
    terminal_printf(" ESI=0x%lx EDI=0x%lx EBP=0x%lx\n", (unsigned long)frame->esi, (unsigned long)frame->edi, (unsigned long)frame->ebp);
    terminal_printf(" DS=0x%lx ES=0x%lx FS=0x%lx GS=0x%lx\n", (unsigned long)frame->ds, (unsigned long)frame->es, (unsigned long)frame->fs, (unsigned long)frame->gs);
    terminal_printf("-----------------------------------\n");
    terminal_write(" System Halted.\n");

    while (1) { asm volatile ("cli; hlt"); }
}

/**
 * @brief Common C interrupt dispatcher called by assembly stubs.
 */
 // <<< FIXED: Use isr_frame_t* consistently >>>
void isr_common_handler(isr_frame_t* frame) {
    if (!frame) { KERNEL_PANIC_HALT("isr_common_handler received NULL frame!"); }
    if (frame->int_no >= IDT_ENTRIES) {
        serial_write("[IDT ERROR] Invalid vector number received by C dispatcher!\n");
        default_isr_handler(frame); // Attempt to print debug info
        KERNEL_PANIC_HALT("Invalid vector number received!");
    }

    interrupt_handler_info_t* entry = &interrupt_c_handlers[frame->int_no];

    if (entry->handler != NULL) {
        entry->handler(frame); // Call registered handler
    } else {
        if (frame->int_no == 8) { // Double Fault
             serial_write("\n*** DOUBLE FAULT *** System Halted.\n");
             default_isr_handler(frame); // Print info
             while(1) asm volatile("cli; hlt");
        }
        default_isr_handler(frame); // Handle other unassigned interrupts
    }

    // <<< FIXED: Re-added EOI call for hardware IRQs >>>
    // Send EOI *after* the handler has run for hardware IRQs (vectors 32-47)
    if (frame->int_no >= PIC1_START_VECTOR && frame->int_no < PIC2_START_VECTOR + 8) {
       pic_send_eoi(frame->int_no);
    }
}


//----------------------------------------------------------------------------
// Public Initialization Function
//----------------------------------------------------------------------------

/**
 * @brief Initializes the IDT, remaps the PIC, sets up gates, and loads the IDTR.
 */
void idt_init(void) {
    terminal_write("[IDT] Initializing IDT and PIC...\n");
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));

    idtp.limit = sizeof(idt_entries) - 1;
    idtp.base  = (uintptr_t)idt_entries;

    pic_remap();

    terminal_write("[IDT] Registering Exception handlers (ISRs 0-19)...\n");
    idt_set_gate_internal(0, (uint32_t)isr0, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(1, (uint32_t)isr1, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(2, (uint32_t)isr2, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(3, (uint32_t)isr3, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(4, (uint32_t)isr4, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(5, (uint32_t)isr5, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(6, (uint32_t)isr6, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(7, (uint32_t)isr7, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(8, (uint32_t)isr8, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(10, (uint32_t)isr10, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(11, (uint32_t)isr11, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(12, (uint32_t)isr12, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(13, (uint32_t)isr13, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(14, (uint32_t)isr14, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(16, (uint32_t)isr16, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(17, (uint32_t)isr17, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(18, (uint32_t)isr18, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(19, (uint32_t)isr19, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);

    terminal_write("[IDT] Registering Hardware Interrupt handlers (IRQs -> Vectors 32-47)...\n");
    idt_set_gate_internal(IRQ0_VECTOR + 0, (uint32_t)irq0, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 1, (uint32_t)irq1, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 2, (uint32_t)irq2, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 3, (uint32_t)irq3, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 4, (uint32_t)irq4, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 5, (uint32_t)irq5, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 6, (uint32_t)irq6, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 7, (uint32_t)irq7, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 8, (uint32_t)irq8, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 9, (uint32_t)irq9, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 10, (uint32_t)irq10, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 11, (uint32_t)irq11, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 12, (uint32_t)irq12, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 13, (uint32_t)irq13, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 14, (uint32_t)irq14, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    idt_set_gate_internal(IRQ0_VECTOR + 15, (uint32_t)irq15, KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);

    terminal_write("[IDT] Registering System Call handler...\n");
    idt_set_gate_internal(SYSCALL_VECTOR, (uint32_t)syscall_handler_asm, KERNEL_CS_SELECTOR, IDT_FLAG_SYSCALL_GATE);
    terminal_printf("[IDT] Registered syscall handler at interrupt 0x%x\n", SYSCALL_VECTOR);

    terminal_write("[IDT] Registering ATA Primary IRQ handler (Vector 46).\n");
    // Assuming ata_primary_irq_handler takes isr_frame_t*
    register_int_handler(IRQ14_VECTOR, ata_primary_irq_handler, NULL);

    terminal_printf("[IDT DEBUG] Loading IDTR: Limit=0x%hx Base=0x%lx (Virt Addr)\n",
                    idtp.limit, (unsigned long)idtp.base);
    idt_flush((uintptr_t)&idtp);

    terminal_write("[IDT] IDT initialized and loaded.\n");

    pic_unmask_required_irqs();

    terminal_write("[IDT] Setup complete.\n");
}