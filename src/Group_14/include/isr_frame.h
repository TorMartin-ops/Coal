/* include/isr_frame.h - CORRECTED Canonical Stack Frame Definition v4.5 */
#ifndef ISR_FRAME_H
#define ISR_FRAME_H

#include <libc/stdint.h> // Use standard integer types

/**
 * @brief Structure representing the stack frame layout created by common
 * interrupt/exception/syscall assembly stubs.
 *
 * Layout MUST EXACTLY MATCH the push order:
 * PUSH Segments -> PUSHA -> CALL C Handler
 * Fields ordered by INCREASING stack address (matches pusha hardware order).
 * Offsets shown are relative to ESP *after* pusha executes.
 */
typedef struct __attribute__((packed)) isr_frame {
    /* Pushed by PUSHA instruction (EDI pushed first, EAX pushed last) */
    uint32_t edi;          /* Offset +0 relative to ESP after pusha */
    uint32_t esi;          /* Offset +4 */
    uint32_t ebp;          /* Offset +8 */
    uint32_t esp_dummy;    /* Offset +12 (ESP before PUSHA) */
    uint32_t ebx;          /* Offset +16 (Arg 1) */
    uint32_t edx;          /* Offset +20 (Arg 3) */
    uint32_t ecx;          /* Offset +24 (Arg 2) */
    uint32_t eax;          /* Offset +28 (Syscall Number / Return Value) */

    /* Pushed manually BEFORE pusha */
    uint32_t gs;           /* Offset +32 */
    uint32_t fs;           /* Offset +36 */
    uint32_t es;           /* Offset +40 */
    uint32_t ds;           /* Offset +44 */

    /* Pushed manually by specific stub BEFORE segments/pusha */
    uint32_t int_no;       /* Offset +48 */
    uint32_t err_code;     /* Offset +52 */

    /* CPU Pushed State (Highest addresses on stack) */
    uint32_t eip;          /* Offset +56 */
    uint32_t cs;           /* Offset +60 */
    uint32_t eflags;       /* Offset +64 */
    uint32_t useresp;      /* Offset +68 (If CPL change) */
    uint32_t ss;           /* Offset +72 (If CPL change) */
} isr_frame_t;

// Define syscall_regs_t as an alias for compatibility if needed elsewhere,
// though standardizing on isr_frame_t is recommended.
typedef isr_frame_t syscall_regs_t;

#endif // ISR_FRAME_H