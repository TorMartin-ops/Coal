; uaccess.asm - raw copy loops between kernel and user memory.
;
; size_t _raw_copy_{from,to}_user(void *dst, const void *src, size_t n)
; Returns the number of bytes not copied. A page fault at .copy is redirected
; to .done through the exception table; rep movsb keeps ECX equal to the
; bytes still outstanding, so the same code serves both directions.

BITS 32

SECTION .text
GLOBAL _raw_copy_from_user
GLOBAL _raw_copy_to_user

%macro EX_TABLE 2
    SECTION .ex_table align=4
    dd %1                       ; faulting instruction
    dd %2                       ; where to resume
    SECTION .text
%endmacro

_raw_copy_from_user:
_raw_copy_to_user:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    cld
    EX_TABLE .copy, .done
.copy:
    rep movsb
.done:
    mov eax, ecx
    pop edi
    pop esi
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
