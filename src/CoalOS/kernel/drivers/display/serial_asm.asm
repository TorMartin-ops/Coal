; Early/crash-path COM1 output usable from assembly stubs without a stack frame
; or any C runtime. Polls the transmitter; clobbers nothing.
;   AL = character to send

section .text
global serial_putc_asm

COM1_DATA equ 0x3F8
COM1_LSR  equ 0x3FD

serial_putc_asm:
    push edx
    push eax
    mov dx, COM1_LSR
.wait:
    in al, dx
    test al, 0x20           ; THR empty
    jz .wait
    pop eax
    mov dx, COM1_DATA
    out dx, al
    pop edx
    ret
