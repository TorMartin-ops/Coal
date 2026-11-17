; _start: the kernel enters with esp -> argc, argv[], NULL, envp[], NULL, auxv
; exactly like Linux i386. __libc_start() never returns.

bits 32

global _start
extern __libc_start

section .text

_start:
    xor ebp, ebp
    mov eax, esp
    and esp, -16
    push eax
    call __libc_start
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
