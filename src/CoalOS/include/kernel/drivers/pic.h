/**
 * @file pic.h
 * @brief 8259 PIC end-of-interrupt helper shared by all IRQ drivers.
 */
#ifndef KERNEL_DRIVERS_PIC_H
#define KERNEL_DRIVERS_PIC_H

#include <kernel/cpu/idt.h> /* PIC1_COMMAND, PIC2_COMMAND, PIC_EOI, IRQn_VECTOR */

/** Acknowledge IRQ line 0-15. Lines 8-15 also need an EOI on the master. */
static inline void pic_send_eoi(unsigned irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

/** Mask IRQ line 0-15. */
static inline void pic_mask(unsigned irq) {
    uint16_t port = (irq >= 8) ? PIC2_DATA : PIC1_DATA;
    outb(port, (uint8_t)(inb(port) | (1u << (irq & 7))));
}

/** Unmask IRQ line 0-15 (and the cascade line for the slave). */
static inline void pic_unmask(unsigned irq) {
    uint16_t port = (irq >= 8) ? PIC2_DATA : PIC1_DATA;
    outb(port, (uint8_t)(inb(port) & ~(1u << (irq & 7))));
    if (irq >= 8) {
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
    }
}

#endif /* KERNEL_DRIVERS_PIC_H */
