; int 0x80 entry. Linux i386 ABI: eax = number, ebx ecx edx esi edi ebp =
; arguments, result or -errno comes back in eax.
;
; The gate is an interrupt gate, so IF is clear for the whole call. The frame
; is an isr_frame_t, which fork/execve need to build or rewrite the user context.

bits 32
default rel

%include "kernel/cpu/cpu_defs.inc"

extern syscall_dispatch         ; void syscall_dispatch(isr_frame_t *)
extern schedule
extern g_need_reschedule

section .text
global syscall_entry

syscall_entry:
    cld                         ; the ABI guarantees DF=0 only on function entry
    push    dword 0             ; err_code
    push    dword 0x80          ; int_no
    push    ds
    push    es
    push    fs
    push    gs
    pusha
    mov     ax, KERNEL_DS       ; fs/gs stay as the user left them (TLS); the kernel never uses them
    mov     ds, ax
    mov     es, ax

    push    esp
    call    syscall_dispatch    ; stores the result in the frame's eax
    add     esp, 4

    cmp     byte [g_need_reschedule], 0
    je      .return
    mov     byte [g_need_reschedule], 0
    call    schedule
.return:
    RESTORE_FRAME
    iret
