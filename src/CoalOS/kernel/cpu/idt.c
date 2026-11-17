/**
 * @file idt.c
 * @brief IDT setup, 8259 PIC management and interrupt dispatch.
 */

#include <kernel/cpu/idt.h>
#include <kernel/cpu/gdt.h>
#include <kernel/core/types.h>
#include <kernel/process/signal.h>
#include <kernel/lib/string.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/storage/block_device.h>

#define IDT_FLAG_INTERRUPT_GATE 0x8E   // ring 0, present, 32-bit interrupt gate
#define IDT_FLAG_SYSCALL_GATE   0xEF   // ring 3 callable trap gate: system calls run with interrupts on

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01
#define PIC_READ_ISR 0x0B              // OCW3: read the in-service register

static struct idt_entry idt_entries[IDT_ENTRIES] __attribute__((aligned(16)));
static struct idt_ptr idtp;
static interrupt_handler_info_t interrupt_c_handlers[IDT_ENTRIES];

// Entry points for vectors 0..47 (0 = none), from isr_stubs.asm.
#define STUB_VECTORS 48
extern void (*const isr_stub_table[STUB_VECTORS])(void);
extern void syscall_entry(void);
extern void idt_flush(uintptr_t idt_ptr_addr);

//============================================================================
// PIC
//============================================================================
static void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, PIC1_START_VECTOR); io_wait();
    outb(PIC2_DATA, PIC2_START_VECTOR); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();   // slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();   // cascade identity
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

static void pic_send_eoi(uint32_t vector) {
    if (vector >= PIC2_START_VECTOR)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

// Unmask IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade) and IRQ14 (ATA).
static void pic_unmask_required_irqs(void) {
    uint8_t master = inb(PIC1_DATA) & ~((1 << 0) | (1 << 1) | (1 << 2));
    uint8_t slave  = inb(PIC2_DATA) & ~(1 << (14 - 8));
    outb(PIC1_DATA, master); io_wait();
    outb(PIC2_DATA, slave);  io_wait();
    if ((inb(PIC1_DATA) & (1 << 1)) != 0)
        KERNEL_PANIC_HALT("Failed to unmask the keyboard IRQ");
}

// IRQ7 and IRQ15 can be spurious: the line dropped before the CPU
// acknowledged it. A spurious IRQ must not be acknowledged; for IRQ15 the
// master still saw a real IRQ2 and needs its EOI.
static void spurious_irq_handler(isr_frame_t *frame) {
    bool slave = frame->int_no == IRQ15_VECTOR;
    uint16_t port = slave ? PIC2_COMMAND : PIC1_COMMAND;

    outb(port, PIC_READ_ISR);
    if (inb(port) & 0x80) {
        pic_send_eoi(frame->int_no);   // genuine IRQ with no driver: ignore it
    } else if (slave) {
        outb(PIC1_COMMAND, PIC_EOI);
    }
}

//============================================================================
// IDT
//============================================================================
static void idt_set_gate(uint8_t num, void (*handler)(void), uint8_t flags) {
    uint32_t base = (uint32_t)handler;
    idt_entries[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt_entries[num].sel       = KERNEL_CODE_SELECTOR;
    idt_entries[num].null      = 0;
    idt_entries[num].flags     = flags;
    idt_entries[num].base_high = (uint16_t)(base >> 16);
}

void register_int_handler(int vector, int_handler_t handler, void *data) {
    KERNEL_ASSERT(vector >= 0 && vector < IDT_ENTRIES, "register_int_handler: invalid vector");
    KERNEL_ASSERT(handler != NULL, "register_int_handler: NULL handler");
    interrupt_c_handlers[vector].handler = handler;
    interrupt_c_handlers[vector].data    = data;
}

static void dump_frame(const isr_frame_t *f) {
    kprintf("\n*** KERNEL PANIC: unhandled exception %u ***", f->int_no);
    kprintf(" err=%#x eip=%#010x cs=%#x eflags=%#x", f->err_code, f->eip, f->cs, f->eflags);
    if (f->cs & 3) kprintf(" user esp=%#010x ss=%#x", f->useresp, f->ss);
    kprintf(" eax=%08x ebx=%08x ecx=%08x edx=%08x", f->eax, f->ebx, f->ecx, f->edx);
    kprintf(" esi=%08x edi=%08x ebp=%08x", f->esi, f->edi, f->ebp);
}

// Page fault in kernel mode that no exception-table entry covers.
void kernel_fault_panic(isr_frame_t *frame) {
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("cli");
    dump_frame(frame);
    kprintf(" cr2=%08x", cr2);
    for (;;)
        asm volatile("hlt");
}

// Called by the stubs for every vector except page faults and syscalls.
void isr_common_handler(isr_frame_t *frame) {
    interrupt_handler_info_t *entry = &interrupt_c_handlers[frame->int_no];

    if (entry->handler) {
        entry->handler(frame);          // hardware handlers send their own EOI
        if (frame->cs & 3) signal_deliver(frame);   // a user-mode task about to resume
        return;
    }
    if (frame->int_no >= IRQ0_VECTOR && frame->int_no < IRQ0_VECTOR + 16) {
        pic_send_eoi(frame->int_no);    // no driver for this IRQ: acknowledge and ignore
        return;
    }
    asm volatile("cli");
    dump_frame(frame);
    for (;;)
        asm volatile("hlt");
}

void idt_init(void) {
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));
    idtp.limit = sizeof(idt_entries) - 1;
    idtp.base  = (uintptr_t)&idt_entries[0];

    pic_remap();

    for (int v = 0; v < STUB_VECTORS; v++) {
        if (isr_stub_table[v])
            idt_set_gate(v, isr_stub_table[v], IDT_FLAG_INTERRUPT_GATE);
    }
    idt_set_gate(SYSCALL_VECTOR, syscall_entry, IDT_FLAG_SYSCALL_GATE);

    register_int_handler(IRQ7_VECTOR, spurious_irq_handler, NULL);
    register_int_handler(IRQ15_VECTOR, spurious_irq_handler, NULL);
    register_int_handler(IRQ14_VECTOR, ata_primary_irq_handler, NULL);

    idt_flush((uintptr_t)&idtp);
    pic_unmask_required_irqs();
}
