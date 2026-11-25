; SYSCALL entry. Linux x86_64 ABI: rax = number, rdi rsi rdx r10 r8 r9 = arguments, result or -errno in rax;
; the CPU leaves the user rip in rcx and rflags in r11 and does not touch rsp, so the entry swaps to the
; kernel stack by hand. It builds the same isr_frame_t as an interrupt, which fork/execve/signals read and
; rewrite, and returns with sysret when it can and with iretq (interrupt_return) otherwise.

bits 64
default rel

%include "kernel/cpu/cpu_defs.inc"

extern syscall_dispatch         ; void syscall_dispatch(isr_frame_t *)
extern schedule
extern g_need_reschedule
extern interrupt_return

SYSCALL_INT_NO equ 0x100

section .text
global syscall_entry

syscall_entry:
    swapgs
    mov     [gs:PC_USER_RSP], rsp
    mov     rsp, [gs:PC_KERNEL_RSP]
    push    qword USER_DS
    push    qword [gs:PC_USER_RSP]
    push    r11                 ; rflags
    push    qword USER_CS
    push    rcx                 ; rip
    push    qword 0             ; err_code
    push    qword SYSCALL_INT_NO
    PUSH_GPRS
    sti                         ; FMASK cleared IF: a blocked call must not starve the timer

    mov     rdi, rsp
    call    syscall_dispatch    ; stores the result in the frame's rax

    cmp     byte [g_need_reschedule], 0
    je      .check_return
    mov     byte [g_need_reschedule], 0
    call    schedule
.check_return:
    cli
    ; sysret only for a plain return to ring 3: user segments, a canonical rip below the user limit
    ; (sysret with a bad rip faults in ring 0) and no single-stepping.
    mov     rcx, [rsp + F_RIP]
    mov     r11, [rsp + F_RFLAGS]
    cmp     qword [rsp + F_CS], USER_CS
    jne     interrupt_return
    cmp     qword [rsp + F_SS], USER_DS
    jne     interrupt_return
    mov     rax, rcx
    shr     rax, 47
    jnz     interrupt_return
    test    r11, EFLAGS_TF | EFLAGS_RF
    jnz     interrupt_return
    ; sysret loads rcx = rip and r11 = rflags. A frame whose saved rcx or r11 differ (rt_sigreturn
    ; restoring an interrupted context) must keep them, so it returns with iretq.
    cmp     rcx, [rsp + F_RCX]
    jne     interrupt_return
    cmp     r11, [rsp + F_R11]
    jne     interrupt_return

    POP_GPRS                    ; rcx and r11 are reloaded below
    add     rsp, 16
    pop     rcx                 ; rip
    add     rsp, 8              ; cs
    pop     r11                 ; rflags
    pop     rsp                 ; user rsp
    swapgs
    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
