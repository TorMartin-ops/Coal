; CPU exception (0-31) and hardware IRQ (32-47, local APIC) entry stubs.
;
; Every stub builds the frame described by isr_frame_t: a dummy error code where the CPU pushes none, the
; vector, then the 15 general registers. An entry from user mode executes swapgs first, so %gs is the
; kernel's per-CPU base from there on. NMI, #DF and #MC can arrive between a swapgs and the instruction
; that needed it (and on their own IST stack), so they decide from the GS base itself, not from CS.

bits 64
default rel

%include "kernel/cpu/cpu_defs.inc"

extern isr_common_handler       ; void isr_common_handler(isr_frame_t *)
extern isr_paranoid_handler

section .text

%macro ISR_NOERR 1
isr%1:
    push    qword 0
    push    qword %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push    qword %1
    jmp     isr_common
%endmacro

%macro ISR_PARANOID 1
isr%1:
    %if %1 = 8
    push    qword %1
    %else
    push    qword 0
    push    qword %1
    %endif
    jmp     isr_paranoid
%endmacro

%macro IRQ 1
irq%1:
    push    qword 0
    push    qword IRQ_BASE + %1
    jmp     isr_common
%endmacro

%assign v 0
%rep 32
    %if v = 2 || v = 8 || v = 18
        ISR_PARANOID v
    %elif v = 10 || v = 11 || v = 12 || v = 13 || v = 14 || v = 17 || v = 21 || v = 29 || v = 30
        ISR_ERR v
    %else
        ISR_NOERR v
    %endif
%assign v v+1
%endrep

%assign i 0
%rep 16
    IRQ i
%assign i i+1
%endrep
IRQ 16                          ; local APIC timer (vector 0x30)
IRQ 31                          ; local APIC spurious interrupt (vector 0x3F)

isr_common:
    cld
    PUSH_GPRS
    test    byte [rsp + F_CS], 3
    jz      .in_kernel
    swapgs
.in_kernel:
    mov     rdi, rsp
    call    isr_common_handler

; Leaves the kernel with iretq. Every path that returns through a frame ends here: interrupts and
; exceptions, system calls that cannot use sysret, and the first run of a new task (ret_from_fork).
global interrupt_return
interrupt_return:
    cli
    test    byte [rsp + F_CS], 3
    jz      .to_kernel
    swapgs
.to_kernel:
    POP_GPRS
    add     rsp, 16             ; int_no, err_code
    iretq

isr_paranoid:
    cld
    PUSH_GPRS
    xor     ebx, ebx            ; rbx: nonzero if swapgs has to be undone
    mov     ecx, MSR_GS_BASE
    rdmsr
    test    edx, edx
    js      .gs_is_kernel       ; a kernel address: already the per-CPU base
    swapgs
    mov     ebx, 1
.gs_is_kernel:
    mov     rdi, rsp
    call    isr_paranoid_handler
    test    ebx, ebx
    jz      .done
    swapgs
.done:
    POP_GPRS
    add     rsp, 16
    iretq

; New tasks "return" here with an isr_frame_t above the alignment slot scheduler_make_context leaves, so the
; first run of a process is an ordinary return to user mode.
global ret_from_fork
ret_from_fork:
    add     rsp, 8
    jmp     interrupt_return

; Entry points indexed by vector 0..63; 0 marks vectors without a stub.
section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 64
    %if v >= 48 && v != 48 && v != 63
        dq 0
    %elif v < 32
        dq isr %+ v
    %else
        %assign n v - 32
        dq irq %+ n
    %endif
%assign v v+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
