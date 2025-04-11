; src/isr_stubs.asm
; Defines basic Interrupt Service Routine (ISR) stubs for exceptions 0-19.
; For exceptions that push an error code, the stub adjusts the stack accordingly.
; ISR 14 (Page Fault) is handled separately in isr_pf.asm

section .text

; External C handler function
extern int_handler

; Export ISR symbols for use in IDT setup
; *** REMOVED isr14 from this list ***
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8
global isr10, isr11, isr12, isr13
global isr16, isr17, isr18, isr19

; Common macro for ISRs without an error code
%macro ISR_NOERRCODE 1
isr%1:
    pusha          ; Save all general-purpose registers
    push dword %1  ; Push the interrupt number
    call int_handler ; Call the C handler
    add esp, 4     ; Pop the interrupt number
    popa           ; Restore registers
    iret           ; Return from interrupt
%endmacro

; Common macro for ISRs WITH an error code pushed by the CPU
%macro ISR_ERRCODE 1
isr%1:
    pusha          ; Save all general-purpose registers
    push dword %1  ; Push the interrupt number
    call int_handler ; Call the C handler (expects error code below interrupt number)
    add esp, 4     ; Pop the interrupt number
    popa           ; Restore registers
    add esp, 4     ; Pop the error code pushed by the CPU
    iret           ; Return from interrupt
%endmacro

; Define ISRs using the macros
ISR_NOERRCODE 0   ; Divide By Zero Exception
ISR_NOERRCODE 1   ; Debug Exception
ISR_NOERRCODE 2   ; Non Maskable Interrupt Exception
ISR_NOERRCODE 3   ; Breakpoint Exception
ISR_NOERRCODE 4   ; Into Detected Overflow Exception
ISR_NOERRCODE 5   ; Out of Bounds Exception
ISR_NOERRCODE 6   ; Invalid Opcode Exception
ISR_NOERRCODE 7   ; No Coprocessor Exception

ISR_ERRCODE   8   ; Double Fault Exception (err code)

; ISR 9 is reserved (Coprocessor Segment Overrun)

ISR_ERRCODE   10  ; Invalid TSS Exception (err code)
ISR_ERRCODE   11  ; Segment Not Present Exception (err code)
ISR_ERRCODE   12  ; Stack Fault Exception (err code)
ISR_ERRCODE   13  ; General Protection Fault Exception (err code)

; *** REMOVED isr14 definition from this file ***
; It should be defined globally in src/isr_pf.asm

; ISR 15 is reserved

ISR_NOERRCODE 16  ; Floating Point Exception

ISR_ERRCODE   17  ; Alignment Check Exception (err code)
ISR_ERRCODE   18  ; Machine Check Exception (err code)

ISR_NOERRCODE 19  ; SIMD Floating-Point Exception

; ISRs 20-31 are reserved by Intel.