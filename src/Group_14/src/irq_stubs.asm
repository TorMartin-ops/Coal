; Exports IRQ handlers
 global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
 global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

 ; External C handler function
 extern int_handler
 ; External ASM function for serial output (optional logging)
 ; extern serial_putc_asm ; Can be commented out if not used elsewhere

 section .text

 ; Common macro for defining IRQ handlers with the fix
 ; Logging removed
 %macro IRQ_HANDLER 1
 irq%1:
     pusha          ; Save all general purpose registers FIRST

     push dword 32 + %1 ; Push the vector number (IRQ 0 = 32, etc.)

     ; ---> FIX: Pass ESP as argument to C handler <---
     mov eax, esp   ; Copy current stack pointer (points to saved state + vector)
     push eax       ; Push ESP onto stack as the argument for int_handler
     call int_handler ; Call the C handler (now receives pointer in 'regs')
     add esp, 4     ; Clean up the pushed ESP argument
     ; ---> END FIX <---

     add esp, 4     ; Clean up the pushed vector number
     popa           ; Restore all general purpose registers
     iret           ; Return from interrupt
 %endmacro

 ; Define IRQ handlers 0 through 15 using the macro
 IRQ_HANDLER 0   ; Timer
 IRQ_HANDLER 1   ; Keyboard
 IRQ_HANDLER 2   ; Cascade
 IRQ_HANDLER 3   ; COM2
 IRQ_HANDLER 4   ; COM1
 IRQ_HANDLER 5   ; LPT2
 IRQ_HANDLER 6   ; Floppy Disk
 IRQ_HANDLER 7   ; LPT1 / Spurious
 IRQ_HANDLER 8   ; RTC
 IRQ_HANDLER 9   ; Free / ACPI SCI
 IRQ_HANDLER 10  ; Free
 IRQ_HANDLER 11  ; Free
 IRQ_HANDLER 12  ; PS/2 Mouse
 IRQ_HANDLER 13  ; FPU Coprocessor
 IRQ_HANDLER 14  ; Primary ATA Hard Disk
 IRQ_HANDLER 15  ; Secondary ATA Hard Disk