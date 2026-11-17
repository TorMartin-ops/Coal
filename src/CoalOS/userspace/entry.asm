; entry.asm - user program entry point
;
; Calls main(argc, argv) and passes its return value to the exit system call.
; The kernel does not build an argument vector yet, so main always sees
; argc = 0 and argv = NULL.

bits 32

global _start
extern main

SYS_EXIT equ 1

section .text

_start:
    xor ebp, ebp        ; mark the outermost stack frame
    push dword 0        ; argv
    push dword 0        ; argc
    call main           ; return value in eax

    mov ebx, eax        ; exit status
    mov eax, SYS_EXIT
    int 0x80

.hang:                  ; exit does not return; stop here if it does
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
