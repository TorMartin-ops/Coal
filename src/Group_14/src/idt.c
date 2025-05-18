/**
 * @file idt.c
 * @brief Interrupt Descriptor Table (IDT) and PIC Management for UiAOS
 * @version 4.3.3 (Corrected diagnostic printf functions to terminal_printf)
 */

//============================================================================
// Includes
//============================================================================

#include <idt.h>
#include <types.h>
#include <string.h>
#include <serial.h>
#include <assert.h>
#include <terminal.h>
#include <block_device.h>

//============================================================================
// Definitions and Constants
//============================================================================
#define IRQ0_VECTOR  (PIC1_START_VECTOR + 0)
#define IRQ1_VECTOR  (PIC1_START_VECTOR + 1)
#define IRQ2_VECTOR  (PIC1_START_VECTOR + 2)
#define IRQ14_VECTOR (PIC2_START_VECTOR + 6)
#define SYSCALL_VECTOR 0x80
#define IDT_FLAG_INTERRUPT_GATE 0x8E
#define IDT_FLAG_TRAP_GATE      0x8F
#define IDT_FLAG_SYSCALL_GATE   0xEE
#define KERNEL_CS_SELECTOR 0x08
#define ICW1_INIT        0x10
#define ICW1_ICW4        0x01
#define ICW4_8086        0x01

//============================================================================
// Module Static Data
//============================================================================
static struct idt_entry idt_entries[IDT_ENTRIES] __attribute__((aligned(16)));
static struct idt_ptr idtp;
static interrupt_handler_info_t interrupt_c_handlers[IDT_ENTRIES];

//============================================================================
// External Assembly Routines
//============================================================================
extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  /* ISR 9 reserved */ extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); /* ISR 15 reserved */
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();

extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

extern void syscall_handler_asm();
extern void idt_flush(uintptr_t idt_ptr_addr);

//============================================================================
// PIC (8259 Programmable Interrupt Controller) Management
//============================================================================
static void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, PIC1_START_VECTOR); io_wait();
    outb(PIC2_DATA, PIC2_START_VECTOR); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
    terminal_write("[IDT] PIC remapped.\n");
}

static void pic_send_eoi(uint32_t vector) {
    if (vector >= PIC2_START_VECTOR && vector < PIC2_START_VECTOR + 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

static void pic_unmask_required_irqs(void) {
    terminal_write("[PIC] Unmasking required IRQs (IRQ0-Timer, IRQ1-Keyboard, IRQ2-Cascade, IRQ14-ATA)...\n");
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    terminal_printf("  [PIC] Current masks before unmask: Master=0x%02x, Slave=0x%02x\n", mask1, mask2);

    uint8_t new_mask1 = mask1 & ~((1 << 0) | (1 << 1) | (1 << 2));
    uint8_t new_mask2 = mask2 & ~(1 << (14 - 8));

    terminal_printf("  [PIC DEBUG] Calculated new_mask1 to be written: 0x%02x (from initial 0x%02x)\n", new_mask1, mask1);
    terminal_printf("  [PIC DEBUG] Calculated new_mask2 to be written: 0x%02x (from initial 0x%02x)\n", new_mask2, mask2);
    terminal_printf("  [PIC] Writing new masks: Master=0x%02x, Slave=0x%02x\n", new_mask1, new_mask2);

    outb(PIC1_DATA, new_mask1); io_wait();
    outb(PIC2_DATA, new_mask2); io_wait();

    uint8_t final_mask1 = inb(PIC1_DATA);
    uint8_t final_mask2 = inb(PIC2_DATA);
    terminal_printf("  [PIC] Read back masks: Master=0x%02x, Slave=0x%02x\n", final_mask1, final_mask2);

    if ((final_mask1 & (1 << 1)) != 0) {
        terminal_printf("  [PIC ERROR] IRQ1 (Keyboard) is STILL MASKED! final_mask1 = 0x%02x\n", final_mask1);
        KERNEL_PANIC_HALT("Failed to unmask IRQ1 on Master PIC!");
    } else {
        terminal_printf("  [PIC INFO] IRQ1 (Keyboard) is successfully UNMASKED. final_mask1 = 0x%02x\n", final_mask1);
    }
    if ((final_mask1 & (1 << 0)) != 0) {
        terminal_printf("  [PIC WARNING] IRQ0 (PIT) is MASKED! final_mask1 = 0x%02x\n", final_mask1);
    } else {
        terminal_printf("  [PIC INFO] IRQ0 (PIT) is UNMASKED. final_mask1 = 0x%02x\n", final_mask1);
    }
}

//============================================================================
// IDT Gate Setup
//============================================================================
static void idt_set_gate_internal(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags) {
    idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt_entries[num].sel       = selector;
    idt_entries[num].null      = 0;
    idt_entries[num].flags     = flags;
    idt_entries[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
}

//============================================================================
// C Handler Registration and Dispatch
//============================================================================
void register_int_handler(int vector, int_handler_t handler, void* data) {
    KERNEL_ASSERT(vector >= 0 && vector < IDT_ENTRIES, "register_int_handler: Invalid vector");
    KERNEL_ASSERT(handler != NULL, "register_int_handler: NULL handler");
    if (interrupt_c_handlers[vector].handler != NULL) {
        terminal_printf("[IDT WARNING] Re-registering handler for vector %d\n", vector);
    }
    interrupt_c_handlers[vector].handler = handler;
    interrupt_c_handlers[vector].data    = data;
}

void default_isr_handler(isr_frame_t* frame) {
    serial_write("\n*** Unhandled Interrupt/Exception ***\n");
    serial_write(" -> Check terminal output for details.\n");
    terminal_printf("\n*** KERNEL PANIC: Unhandled Interrupt/Exception ***\n");
    KERNEL_ASSERT(frame != NULL, "default_isr_handler received NULL frame!");
    terminal_printf(" Vector:  %lu (0x%lx)\n", (unsigned long)frame->int_no, (unsigned long)frame->int_no);
    terminal_printf(" ErrCode: 0x%lx\n", (unsigned long)frame->err_code);
    terminal_printf(" EIP:     0x%08lx CS:      0x%lx EFLAGS:  0x%lx\n",
                  (unsigned long)frame->eip, (unsigned long)frame->cs, (unsigned long)frame->eflags);
    if ((frame->cs & 0x3) != 0) {
        terminal_printf(" UserESP: 0x%p UserSS:  0x%lx\n", (void*)frame->useresp, (unsigned long)frame->ss);
    }
    if (frame->int_no == 14) {
        uintptr_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_printf(" Fault Address (CR2): 0x%p\n", (void*)cr2);
        terminal_printf(" PF Error Decode: [%s %s %s %s %s]\n",
                      (frame->err_code & 0x1) ? "P" : "NP", (frame->err_code & 0x2) ? "W" : "R",
                      (frame->err_code & 0x4) ? "U" : "S", (frame->err_code & 0x8) ? "RSV" : "-",
                      (frame->err_code & 0x10) ? "I/D" : "-");
    }
    terminal_printf(" EAX=0x%lx EBX=0x%lx ECX=0x%lx EDX=0x%lx\n",
                   (unsigned long)frame->eax, (unsigned long)frame->ebx,
                   (unsigned long)frame->ecx, (unsigned long)frame->edx);
    terminal_printf(" ESI=0x%lx EDI=0x%lx EBP=0x%p\n",
                   (unsigned long)frame->esi, (unsigned long)frame->edi, (void*)frame->ebp);
    terminal_write(" System Halted.\n");
    while (1) { asm volatile ("cli; hlt"); }
}

void isr_common_handler(isr_frame_t* frame) {
    if (!frame) { KERNEL_PANIC_HALT("isr_common_handler received NULL frame!"); }
    uint32_t vector = frame->int_no;
    if (vector >= IDT_ENTRIES) {
        terminal_printf("[IDT ERROR] Invalid vector 0x%x in C dispatcher!\n", vector);
        default_isr_handler(frame);
        KERNEL_PANIC_HALT("Invalid vector number processed by default handler!");
    }
    interrupt_handler_info_t* entry = &interrupt_c_handlers[vector];
    if (entry->handler != NULL) {
        entry->handler(frame);
    } else {
        default_isr_handler(frame);
    }
    if (vector >= PIC1_START_VECTOR && vector < PIC2_START_VECTOR + 8) {
       pic_send_eoi(vector);
    }
}

//============================================================================
// Public Initialization Function
//============================================================================
void idt_init(void) {
    terminal_write("[IDT] Initializing IDT and PIC...\n");
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));
    idtp.limit = sizeof(idt_entries) - 1;
    idtp.base  = (uintptr_t)&idt_entries[0];
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
    void (*irq_stub_table[])() = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (int i = 0; i < 16; ++i) {
        idt_set_gate_internal(IRQ0_VECTOR + i, (uint32_t)irq_stub_table[i], KERNEL_CS_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    }

    terminal_write("[IDT] Registering System Call handler...\n");
    idt_set_gate_internal(SYSCALL_VECTOR, (uint32_t)syscall_handler_asm, KERNEL_CS_SELECTOR, IDT_FLAG_SYSCALL_GATE);
    terminal_printf("[IDT] Registered syscall handler at vector 0x%x\n", SYSCALL_VECTOR);

    terminal_write("[IDT] Registering ATA Primary IRQ handler (Vector 46).\n");
    KERNEL_ASSERT(ata_primary_irq_handler != NULL, "ata_primary_irq_handler is NULL");
    register_int_handler(IRQ14_VECTOR, ata_primary_irq_handler, NULL);

    terminal_printf("[IDT] Loading IDTR: Limit=0x%hX Base=%#010lx (Virt Addr)\n",
                    idtp.limit, (unsigned long)idtp.base);
    idt_flush((uintptr_t)&idtp);

    terminal_write("[IDT] IDT initialized and loaded.\n");
    pic_unmask_required_irqs();
    terminal_write("[IDT] Setup complete.\n");
}