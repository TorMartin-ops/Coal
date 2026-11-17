; CPU exception (0-19) and hardware IRQ (32-47) entry stubs.
; Vector 14 lives in isr_pf.asm, vector 0x80 in syscall.asm.

bits 32
default rel

%include "kernel/cpu/cpu_defs.inc"

extern isr_common_handler       ; void isr_common_handler(isr_frame_t *)

section .text

; Exception without a CPU error code: push a dummy one.
%macro ISR_NOERR 1
isr%1:
    push    dword 0
    push    dword %1
    jmp     isr_common_stub
%endmacro

; Exception where the CPU already pushed an error code.
%macro ISR_ERR 1
isr%1:
    push    dword %1
    jmp     isr_common_stub
%endmacro

%macro IRQ 1
irq%1:
    push    dword 0
    push    dword IRQ_BASE + %1
    jmp     isr_common_stub
%endmacro

ISR_NOERR 0

; Debug exception. A user program with TF set that executes sysenter gets a single-step trap in the
; kernel entry (sysenter_entry .. sysenter_flags_done) while the stack is still the scratch stack.
; Return at once: the entry code clears TF itself and the user's TF stays in the saved eflags.
extern sysenter_entry, sysenter_flags_done
isr1:
    cmp     word [esp + 4], KERNEL_CS
    jne     .trap
    cmp     dword [esp], sysenter_entry
    jb      .trap
    cmp     dword [esp], sysenter_flags_done
    ja      .trap
    iret
.trap:
    push    dword 0
    push    dword 1
    jmp     isr_common_stub

ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19

%assign i 0
%rep 16
    IRQ i
%assign i i+1
%endrep

isr_common_stub:
    SAVE_FRAME
    cld                         ; user mode may have set DF; the C ABI and rep movs need it clear
    push    esp                 ; &isr_frame_t
    call    isr_common_handler
    add     esp, 4
    RESTORE_FRAME
    iret

; Entry points indexed by vector 0..47; 0 marks vectors without a stub.
extern isr14
section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 48
    %if v = 9 || v = 15 || (v >= 20 && v < 32)
        dd 0
    %elif v = 14
        dd isr14
    %elif v < 32
        dd isr %+ v
    %else
        %assign n v - 32
        dd irq %+ n
    %endif
%assign v v+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
