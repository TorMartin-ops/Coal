/* include/arch/i386/isr_frame.h */
#ifndef ARCH_I386_ISR_FRAME_H
#define ARCH_I386_ISR_FRAME_H

#include <libc/stdint.h> // Use standard integer types

// Structure representing the stack frame layout created by the common interrupt stub
typedef struct __attribute__((packed)) isr_frame {
    // Pushed by common_interrupt_stub *after* pusha
    uint32_t gs, fs, es, ds;

    // --- CORRECTED ORDER ---
    // Pushed by PUSHA instruction (in standard order: EAX, ECX, EDX, EBX, ESP_orig, EBP, ESI, EDI)
    // The struct fields should match the popa order (reverse of pusha).
    // popad restores: EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy; // The ESP value before PUSHA was executed
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    // --- END CORRECTION ---

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