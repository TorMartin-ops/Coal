; void gdt_flush(uint32_t gdt_ptr): load the GDT and reload all segments.

%include "kernel/cpu/cpu_defs.inc"

global gdt_flush
section .text

gdt_flush:
    mov     eax, [esp + 4]
    lgdt    [eax]
    mov     ax, KERNEL_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    jmp     KERNEL_CS:.done
.done:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
