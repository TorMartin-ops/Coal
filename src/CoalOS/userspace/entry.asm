; _start: the kernel enters with rsp -> argc, argv[], NULL, envp[], NULL, auxv
; exactly like Linux x86_64. __libc_start() never returns.

bits 64

global _start
extern __libc_start

section .text

_start:
    xor ebp, ebp
    mov rdi, rsp
    and rsp, -16
    call __libc_start
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
