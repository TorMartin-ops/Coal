/**
 * @file idt.c
 * @brief IDT setup, 8259 PIC management and interrupt dispatch.
 */

#include <kernel/cpu/idt.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/msr.h>
#include <kernel/cpu/percpu.h>
#include <kernel/memory/paging.h>
#include <kernel/core/types.h>
#include <kernel/process/signal.h>
#include <kernel/process/process.h>
#include <kernel/lib/string.h>
#include <kernel/core/log.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/storage/block_device.h>

#define IDT_FLAG_INTERRUPT_GATE 0x8E   // ring 0, present, 64-bit interrupt gate
#define IDT_FLAG_USER_INT_GATE  0xEE   // int3 may be executed from ring 3

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01
#define PIC_READ_ISR 0x0B              // OCW3: read the in-service register

static struct idt_entry idt_entries[IDT_ENTRIES] __attribute__((aligned(16)));
static interrupt_handler_info_t interrupt_c_handlers[IDT_ENTRIES];

// Entry points for vectors 0..63 (0 = none), from isr_stubs.asm.
#define STUB_VECTORS 64
static uint32_t irq_counts[STUB_VECTORS];
extern void (*const isr_stub_table[STUB_VECTORS])(void);

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
static void idt_set_gate(uint8_t num, void (*handler)(void), uint8_t flags, uint8_t ist) {
    uint64_t base = (uint64_t)handler;
    idt_entries[num] = (struct idt_entry){
        .base_low = (uint16_t)base, .sel = KERNEL_CODE_SELECTOR, .ist = ist, .flags = flags,
        .base_mid = (uint16_t)(base >> 16), .base_high = (uint32_t)(base >> 32),
    };
}

void register_int_handler(int vector, int_handler_t handler, void *data) {
    KERNEL_ASSERT(vector >= 0 && vector < IDT_ENTRIES, "register_int_handler: invalid vector");
    KERNEL_ASSERT(handler != NULL, "register_int_handler: NULL handler");
    interrupt_c_handlers[vector].handler = handler;
    interrupt_c_handlers[vector].data    = data;
}

int_handler_t get_int_handler(int vector) {
    return vector >= 0 && vector < IDT_ENTRIES ? interrupt_c_handlers[vector].handler : NULL;
}

static void dump_frame(const isr_frame_t *f) {
    kprintf("\n*** KERNEL PANIC: unhandled exception %lu ***", (unsigned long)f->int_no);
    kprintf(" err=%#lx rip=%#018lx cs=%#lx rflags=%#lx rsp=%#018lx", (unsigned long)f->err_code, (unsigned long)f->rip,
            (unsigned long)f->cs, (unsigned long)f->rflags, (unsigned long)f->rsp);
    kprintf(" rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx", (unsigned long)f->rax, (unsigned long)f->rbx,
            (unsigned long)f->rcx, (unsigned long)f->rdx);
    kprintf(" rsi=%016lx rdi=%016lx rbp=%016lx cr2=%016lx", (unsigned long)f->rsi, (unsigned long)f->rdi,
            (unsigned long)f->rbp, (unsigned long)read_cr2());
}

// Kernel-mode fault that nothing expected.
void kernel_fault_panic(isr_frame_t *frame) {
    asm volatile("cli");
    dump_frame(frame);
    for (;;)
        asm volatile("hlt");
}

// Signal for a CPU exception raised by user code (as Linux maps them).
static int exception_signal(uint32_t vector) {
    switch (vector) {
    case 0: case 7: case 16: case 19: return SIGFPE;    // divide, no FPU, x87 and SIMD errors
    case 1: case 3:                   return SIGTRAP;   // debug, int3
    case 6:                           return SIGILL;    // ud2 and unknown opcodes
    case 11: case 12: case 17:        return SIGBUS;    // not present, stack, alignment
    default:                          return SIGSEGV;   // #GP, #TS, ...
    }
}

uint32_t irq_count(unsigned vector) {
    return vector < STUB_VECTORS ? irq_counts[vector] : 0;
}

// Called by the stubs for every vector except page faults and syscalls.
void isr_common_handler(isr_frame_t *frame) {
    interrupt_handler_info_t *entry = &interrupt_c_handlers[frame->int_no];

    irq_counts[frame->int_no]++;
    if (entry->handler) {
        entry->handler(frame);          // hardware handlers send their own EOI
    } else if (frame->int_no >= IRQ0_VECTOR && frame->int_no < IRQ0_VECTOR + 16) {
        pic_send_eoi(frame->int_no);    // no driver for this IRQ: acknowledge and ignore
    } else if (frame->cs & 3) {
        if (frame->int_no != 1)         /* a single-stepping program traps after every instruction */
            KLOG_WARN("idt", "pid %u: exception %u at %#lx", (unsigned)get_current_process()->pid,
                      (unsigned)frame->int_no, (unsigned long)frame->rip);
        signal_force(exception_signal(frame->int_no));
    } else {
        kernel_fault_panic(frame);
    }
    if (frame->cs & 3) {
        if (g_need_reschedule) schedule();          // an interrupt made a better task runnable
        signal_return_to_user(frame, SIGNAL_NO_RESTART);
    }
}

// NMI, double fault and machine check run on their own stacks and never return to a task.
void isr_paranoid_handler(isr_frame_t *frame) {
    if (frame->int_no == 2) {
        irq_counts[2]++;                    // nothing in this system raises one: count it and carry on
        return;
    }
    kernel_fault_panic(frame);
}

void idt_init(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtp = { sizeof(idt_entries) - 1, (uint64_t)idt_entries };

    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_c_handlers, 0, sizeof(interrupt_c_handlers));

    pic_remap();

    for (int v = 0; v < STUB_VECTORS; v++) {
        uint8_t ist = v == 8 ? IST_DOUBLE_FAULT : v == 2 ? IST_NMI : v == 18 ? IST_MACHINE_CHECK : 0;
        if (isr_stub_table[v])
            idt_set_gate(v, isr_stub_table[v], v == 3 ? IDT_FLAG_USER_INT_GATE : IDT_FLAG_INTERRUPT_GATE, ist);
    }

    register_int_handler(IRQ7_VECTOR, spurious_irq_handler, NULL);
    register_int_handler(IRQ15_VECTOR, spurious_irq_handler, NULL);
    register_int_handler(IRQ14_VECTOR, ata_primary_irq_handler, NULL);
    register_int_handler(PF_VECTOR, page_fault_handler, NULL);

    asm volatile("lidt %0" : : "m"(idtp));
    pic_unmask_required_irqs();
}
