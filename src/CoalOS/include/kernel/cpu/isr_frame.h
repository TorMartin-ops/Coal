/* Stack frame built by the interrupt, exception and syscall entry stubs. */
#ifndef ISR_FRAME_H
#define ISR_FRAME_H

#include <kernel/core/types.h>

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
    /* PUSHA frame: edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax */
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;
    uint32_t ebx;      // arg1
    uint32_t edx;      // arg3
    uint32_t ecx;      // arg2
    uint32_t eax;      // syscall number on entry, return value on exit

    /* segment registers pushed by stub */
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;

    /* pushed by stub: int_no, err_code */
    uint32_t int_no;
    uint32_t err_code;

    /* hardware-pushed on trap */
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
} isr_frame_t;


#endif // ISR_FRAME_H