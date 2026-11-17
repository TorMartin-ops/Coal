; System call entries. Linux i386 ABI: eax = number, ebx ecx edx esi edi ebp = arguments, result or
; -errno comes back in eax. Both entries build the same isr_frame_t, which fork/execve/signals use to
; read or rewrite the user context, and run with interrupts on so a blocked call never starves the timer.
;
; syscall_entry: int 0x80 (a trap gate), returns with iret.
; sysenter_entry: sysenter from the vDSO's __kernel_vsyscall (include/kernel/cpu/vdso_abi.h). The
; stub keeps its stack pointer in ebp and the sixth argument on that stack; the kernel resumes it at
; the landing pad with sysexit, which needs no descriptor loads and no stack frame pops.

bits 32
default rel

%include "kernel/cpu/cpu_defs.inc"

extern syscall_dispatch         ; void syscall_dispatch(isr_frame_t *)
extern schedule
extern g_need_reschedule
extern tss

%macro EX_TABLE 2
    section .ex_table align=4
    dd %1                       ; faulting instruction
    dd %2                       ; where to resume
    section .text
%endmacro

F_EDI       equ 0               ; isr_frame_t offsets
F_ESI       equ 4
F_EBP       equ 8
F_EBX       equ 16
F_EAX       equ 28
F_ES        equ 40
F_DS        equ 44
F_EIP       equ 56
F_EFLAGS    equ 64
F_USERESP   equ 68
FRAME_SIZE  equ 76
EFLAGS_TF   equ 0x100
EFLAGS_IF   equ 0x200
EFLAGS_NT   equ 0x4000
KERNEL_BASE equ 0xC0000000
EFAULT      equ 14

section .text
global syscall_entry, sysenter_entry, sysenter_flags_done, sysenter_scratch_top

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

; sysenter loads esp from IA32_SYSENTER_ESP, which points at the scratch stack below: the kernel
; stack of the running task is only known through tss.esp0, and updating an MSR on every task switch
; would be slow. The scratch stack only ever holds the three words of a single-step trap (see isr1).
; Interrupts are off and the user's eip, esp, eflags are not saved by the CPU, so ebp has to carry esp.
sysenter_entry:
    mov     esp, [tss + 4]              ; tss.esp0
    push    dword USER_DS
    push    ebp                         ; the user esp
    pushfd
    test    byte [esp + 1], (EFLAGS_TF | EFLAGS_NT) >> 8
    jz      sysenter_flags_done
    push    dword 2                     ; TF would trap every kernel instruction and NT turns iret into a task
    popfd                               ; return: clear them (an interrupt gate does that for int 0x80), keep the saved copy
sysenter_flags_done:
    cld
    or      dword [esp], EFLAGS_IF      ; sysenter cleared IF; user mode always runs with it set
    push    dword USER_CS
    push    dword VDSO_LANDING
    push    dword 0                     ; err_code
    push    dword 0x80                  ; int_no
    push    ds
    push    es
    push    fs
    push    gs
    pusha
    mov     ax, KERNEL_DS
    mov     ds, ax
    mov     es, ax
    sti

    EX_TABLE .sixth, .fault
    cmp     ebp, KERNEL_BASE - 4        ; the stack pointer is the user's word: it must not make the kernel read its own memory
    ja      .fault
.sixth:
    mov     ebp, [ebp]                  ; the stub pushed the user's ebp: the sixth argument
    mov     [esp + F_EBP], ebp
    push    esp
    call    syscall_dispatch
    add     esp, 4
.exit:
    cmp     byte [g_need_reschedule], 0
    je      .leave
    mov     byte [g_need_reschedule], 0
    call    schedule
.leave:
    ; sysexit only for a plain return to the landing pad: it cannot restore ecx, edx or TF, and
    ; a signal handler, execve or sigreturn has set another eip (those take the iret below).
    cmp     dword [esp + F_EIP], VDSO_LANDING
    jne     .iret
    test    byte [esp + F_EFLAGS + 1], EFLAGS_TF >> 8
    jnz     .iret
    mov     ecx, [esp + F_USERESP]
    mov     edx, VDSO_LANDING
    mov     edi, [esp + F_EDI]
    mov     esi, [esp + F_ESI]
    mov     ebx, [esp + F_EBX]
    mov     eax, [esp + F_EAX]
    mov     es, [esp + F_ES]            ; fs and gs were never changed; ebp, ecx and edx come back from the stub
    mov     ds, [esp + F_DS]
    add     esp, FRAME_SIZE
    sti                                 ; takes effect after sysexit
    sysexit
.iret:
    RESTORE_FRAME
    iret
.fault:                                 ; the user stack is unreadable
    mov     dword [esp + F_EAX], -EFAULT
    jmp     .exit

section .bss align=16
    resb    64
sysenter_scratch_top:

section .note.GNU-stack noalloc noexec nowrite progbits
