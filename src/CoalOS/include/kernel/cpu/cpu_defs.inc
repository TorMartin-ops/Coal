; Selectors, the interrupt frame and per-CPU offsets shared by the assembly sources.
; They must match gdt.h, isr_frame.h and percpu.h (checked there with static assertions).

KERNEL_CS   equ 0x08
KERNEL_DS   equ 0x10
USER_DS     equ 0x23        ; ring 3 selectors, see gdt.h
USER_CS     equ 0x2B
IRQ_BASE    equ 32          ; first PIC vector, see include/kernel/cpu/idt.h

MSR_GS_BASE equ 0xC0000101

; isr_frame_t: 15 general registers, int_no, err_code, then what the CPU pushed.
F_R15       equ 0
F_R11       equ 32
F_RBP       equ 64
F_RCX       equ 96
F_RAX       equ 112
F_INT_NO    equ 120
F_ERR       equ 128
F_RIP       equ 136
F_CS        equ 144
F_RFLAGS    equ 152
F_RSP       equ 160
F_SS        equ 168
FRAME_SIZE  equ 176

; struct percpu offsets
PC_KERNEL_RSP equ 8
PC_USER_RSP   equ 16

EFLAGS_TF   equ 0x100
EFLAGS_RF   equ 0x10000
EFAULT      equ 14

; Push the 15 general registers in isr_frame_t order (rax first, r15 ends up lowest).
%macro PUSH_GPRS 0
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
%endmacro

%macro POP_GPRS 0
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
%endmacro

; Exception table entry: a faulting instruction and where to resume (kernel/cpu/exception_table.c).
%macro EX_TABLE 2
    section .ex_table progbits alloc noexec nowrite align=8
    dq %1
    dq %2
    section .text
%endmacro
