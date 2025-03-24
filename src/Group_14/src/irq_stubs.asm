global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

extern int_handler

section .text

irq0:
    pusha
    push dword 32  ; vector 0x20
    call int_handler
    add esp, 4
    popa
    iret

irq1:
    pusha
    push dword 33  ; vector 0x21
    call int_handler
    add esp, 4
    popa
    iret

irq2:
    pusha
    push dword 34  ; vector 0x22
    call int_handler
    add esp, 4
    popa
    iret

irq3:
    pusha
    push dword 35  ; vector 0x23
    call int_handler
    add esp, 4
    popa
    iret

irq4:
    pusha
    push dword 36  ; vector 0x24
    call int_handler
    add esp, 4
    popa
    iret

irq5:
    pusha
    push dword 37  ; vector 0x25
    call int_handler
    add esp, 4
    popa
    iret

irq6:
    pusha
    push dword 38  ; vector 0x26
    call int_handler
    add esp, 4
    popa
    iret

irq7:
    pusha
    push dword 39  ; vector 0x27
    call int_handler
    add esp, 4
    popa
    iret

irq8:
    pusha
    push dword 40  ; vector 0x28
    call int_handler
    add esp, 4
    popa
    iret

irq9:
    pusha
    push dword 41  ; vector 0x29
    call int_handler
    add esp, 4
    popa
    iret

irq10:
    pusha
    push dword 42  ; vector 0x2A
    call int_handler
    add esp, 4
    popa
    iret

irq11:
    pusha
    push dword 43  ; vector 0x2B
    call int_handler
    add esp, 4
    popa
    iret

irq12:
    pusha
    push dword 44  ; vector 0x2C
    call int_handler
    add esp, 4
    popa
    iret

irq13:
    pusha
    push dword 45  ; vector 0x2D
    call int_handler
    add esp, 4
    popa
    iret

irq14:
    pusha
    push dword 46  ; vector 0x2E
    call int_handler
    add esp, 4
    popa
    iret

irq15:
    pusha
    push dword 47  ; vector 0x2F
    call int_handler
    add esp, 4
    popa
    iret
