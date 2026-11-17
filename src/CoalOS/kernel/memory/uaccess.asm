; uaccess.asm - raw copy loops between kernel and user memory.
;
; size_t _raw_copy_{from,to}_user(void *dst, const void *src, size_t n)
; Returns the number of bytes not copied. Whole qwords go first, then the 0..7 tail bytes. A page fault in
; either rep is redirected through the exception table; rep leaves rcx equal to the units still
; outstanding, so the same code serves both directions. With SMAP the access window is opened by stac
; around the copy (g_cpu_smap says whether the CPU has it: stac is an undefined opcode without it).

bits 64
default rel

%include "kernel/cpu/cpu_defs.inc"

extern g_cpu_smap

section .text
global _raw_copy_from_user
global _raw_copy_to_user

_raw_copy_from_user:
_raw_copy_to_user:
    mov     rcx, rdx
    and     edx, 7              ; tail bytes
    shr     rcx, 3              ; qwords
    cmp     byte [g_cpu_smap], 0
    je      .copy
    stac
.copy:
    EX_TABLE .words, .words_fault
.words:
    rep movsq
    mov     rcx, rdx
    EX_TABLE .tail, .done
.tail:
    rep movsb
.done:
    mov     rax, rcx
.leave:
    cmp     byte [g_cpu_smap], 0
    je      .ret
    clac
.ret:
    ret
.words_fault:
    lea     rax, [rdx + rcx * 8]    ; qwords left plus the tail
    jmp     .leave

section .note.GNU-stack noalloc noexec nowrite progbits
