; isr_stubs.asm
global isr0
global isr1
global isr2

; We’ll call a C function named int_handler(int num).
extern int_handler

section .text

; Each ISR pushes the CPU state + “which interrupt” number, calls int_handler, then iret

isr0:
    pusha                 ; push general registers
    push dword 0         ; “interrupt #0”
    call int_handler
    add esp, 4
    popa
    iret

isr1:
    pusha
    push dword 1         ; “interrupt #1”
    call int_handler
    add esp, 4
    popa
    iret

isr2:
    pusha
    push dword 2         ; “interrupt #2”
    call int_handler
    add esp, 4
    popa
    iret
