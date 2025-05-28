; gdt_flush.asm
global gdt_flush
section .text
gdt_flush:
    mov eax, [esp + 4]   ; Load pointer to GDT.
    lgdt [eax]           ; Load GDT into the CPU.

    ; Reload segment registers with new GDT values.
    mov ax, 0x10         ; Data segment selector (index 2 in GDT).
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to load the new code segment (index 1 in GDT).
    jmp 0x08:.flush_done
.flush_done:
    ret