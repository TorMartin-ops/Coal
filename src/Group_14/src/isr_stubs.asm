; src/isr_stubs.asm
; Defines basic Interrupt Service Routine (ISR) stubs for exceptions 0-19.
; Includes Double Fault handler (ISR 8) with debug print.
; Adds debug prints for #TS (10) and #SS (12).

section .text

; External C handler function
extern int_handler
extern serial_putc_asm

; Export ISR symbols for use in IDT setup
; *** REMOVED isr14 ***
; *** ADDED isr8, isr10, isr12 ***
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8
global isr10, isr11, isr12, isr13
global isr16, isr17, isr18, isr19

; Common macro for ISRs without an error code
%macro ISR_NOERRCODE 1
isr%1:
    ; --- DEBUG ---
    ; pusha
    ; mov al, '0' + %1 ; Crude way to print number 0-9
    ; call serial_putc_asm
    ; popa
    ; --- End DEBUG ---
    pusha
    push dword %1
    call int_handler
    add esp, 4
    popa
    iret
%endmacro

; Common macro for ISRs WITH an error code pushed by the CPU
%macro ISR_ERRCODE 1
isr%1:
    ; --- DEBUG Prints for Specific Faults ---
%if %1 == 8 ; Double Fault
    pusha
    mov al, 'D' ; Print 'D' for Double Fault
    call serial_putc_asm
    cli
.halt_df: hlt
    jmp .halt_df
    popa ; Unreachable, but balances stack for assembler
%elif %1 == 10 ; Invalid TSS
    pusha
    mov al, 'T' ; Print 'T' for Invalid TSS
    call serial_putc_asm
    popa
%elif %1 == 11 ; Segment Not Present
    pusha
    mov al, 'N' ; Print 'N' for Segment Not Present (just in case)
    call serial_putc_asm
    popa
%elif %1 == 12 ; Stack Segment Fault
    pusha
    mov al, '#' ; Print '#' for Stack Segment Fault
    call serial_putc_asm
    popa
%elif %1 == 13 ; General Protection Fault
    pusha
    mov al, 'G' ; Print 'G' for GP Fault
    call serial_putc_asm
    popa
%endif
    ; --- End DEBUG ---
    pusha
    push dword %1
    call int_handler
    add esp, 4
    popa
    add esp, 4 ; Pop error code
    iret
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

ISR_ERRCODE   8   ; Double Fault Exception (err code) <<< Prints 'D' & HALTs

; ISR 9 is reserved

ISR_ERRCODE   10  ; Invalid TSS Exception (err code) <<< Prints 'T'
ISR_ERRCODE   11  ; Segment Not Present Exception (err code) <<< Prints 'N'
ISR_ERRCODE   12  ; Stack Fault Exception (err code) <<< Prints '#'
ISR_ERRCODE   13  ; General Protection Fault Exception (err code) <<< Prints 'G'

; *** REMOVED isr14 definition from this file ***

; ISR 15 is reserved

ISR_NOERRCODE 16  ; Floating Point Exception

ISR_ERRCODE   17  ; Alignment Check Exception (err code)
ISR_ERRCODE   18  ; Machine Check Exception (err code)

ISR_NOERRCODE 19  ; SIMD Floating-Point Exception

; ISRs 20-31 are reserved by Intel.