/* Stack frame built by the interrupt, exception and syscall entry stubs. */
#ifndef ISR_FRAME_H
#define ISR_FRAME_H

#include <kernel/core/types.h>

/**
 * The frame is built the same way for every entry into the kernel (see isr_stubs.asm and syscall.asm), so
 * fork, execve and signals can read and rewrite the user context in one place and every exit can leave
 * with iretq. Fields go by increasing stack address: the stubs push rax first and r15 last.
 */
typedef struct isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;     /* rax: syscall number on entry, result on exit */

    uint64_t int_no;        /* vector; 0x100 for a system call */
    uint64_t err_code;      /* from the CPU, else 0 */

    /* hardware-pushed on every entry */
    uint64_t rip, cs, rflags, rsp, ss;
} isr_frame_t;

#define SYSCALL_INT_NO 0x100

_Static_assert(sizeof(isr_frame_t) == 176, "update FRAME_SIZE in cpu_defs.inc");
_Static_assert(offsetof(isr_frame_t, rax) == 112 && offsetof(isr_frame_t, rip) == 136 &&
               offsetof(isr_frame_t, ss) == 168, "update the frame offsets in cpu_defs.inc");

#endif // ISR_FRAME_H
