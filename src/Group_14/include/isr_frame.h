/* include/arch/i386/isr_frame.h */
/* Defines the frame pushed by the corrected interrupt stubs (isr_stub.S / *_stubs.asm) */
#ifndef ARCH_I386_ISR_FRAME_H
#define ARCH_I386_ISR_FRAME_H

#include <libc/stdint.h> // Use standard integer types

// Structure representing the stack frame layout created by the common interrupt stub
// Matches the order of pushes: gs, fs, es, ds, pusha, int_no, err_code, eip, cs, eflags, useresp, ss
typedef struct __attribute__((packed)) isr_frame {
    // Pushed by common_interrupt_stub *after* pusha
    uint32_t gs, fs, es, ds;
    // Pushed by PUSHA instruction (in reverse order)
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    // Pushed by specific ISR/IRQ stub before jumping to common stub
    uint32_t int_no;   // Interrupt (vector) number
    uint32_t err_code; // Error code (pushed by CPU or 0 by stub)
    // Pushed by CPU automatically on interrupt/exception
    uint32_t eip, cs, eflags;
    // Pushed by CPU only on privilege level change (user->kernel)
    uint32_t useresp; // User stack pointer
    uint32_t ss;      // User stack segment
} isr_frame_t;

#endif // ARCH_I386_ISR_FRAME_H