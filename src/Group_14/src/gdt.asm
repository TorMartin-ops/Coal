global gdt_flush
gdt_flush:
    mov eax, [esp + 4]   ; Load pointer to GDT
    lgdt [eax]           ; Load GDT into the CPU

    ; Reload segment registers with new GDT values
    mov ax, 0x10         ; Data segment (index 2 in GDT)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Jump to the new Code segment (index 1 in GDT)
    jmp 0x08:.flush_done

.flush_done:
    ret
