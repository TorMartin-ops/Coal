; Kernel context switch.
;
; simple_switch saves the callee-saved registers and the flags on the old kernel stack, stores that stack
; pointer, loads the new one and unwinds the same frame. The frame layout (low to high) is struct
; switch_frame in scheduler.c: r15 r14 r13 r12 rbp rbx | rflags | return address.

bits 64
default rel

section .text
global simple_switch

; void simple_switch(uint64_t **save_rsp, uint64_t *new_rsp)
simple_switch:
    pushfq
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [rdi], rsp
    mov     rsp, rsi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    popfq
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
