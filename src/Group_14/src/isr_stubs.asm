; isr_stubs.asm

global isr0
global isr1
global isr2

; Tell the assembler “int_handler” is defined elsewhere (in C):
extern int_handler

section .text

isr0:
    pusha
    push dword 0
    call int_handler
    add esp, 4
    popa
    iret

isr1:
    pusha
    push dword 1
    call int_handler
    add esp, 4
    popa
    iret

isr2:
    pusha
    push dword 2
    call int_handler
    add esp, 4
    popa
    iret
