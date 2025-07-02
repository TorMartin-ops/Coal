; idt_flush.asm
; Loads the IDT register (IDTR).
; MODIFIED TO PRINT SIMPLE MARKERS BEFORE/AFTER LIDT

section .text
bits 32

global idt_flush ; Export the symbol for the linker

; External reference to the low-level serial output function
extern serial_putc_asm

; Function signature expected from C:
; void idt_flush(uint32_t idt_ptr_addr);

idt_flush:
    mov eax, [esp + 4]  ; Get the argument (idt_ptr_addr) from the stack

    ; --- BEGIN DEBUG PRINT (BEFORE LIDT) ---
    push eax            ; Save EAX (holds idt_ptr_addr, needed for lidt)
                        ; Also saves EAX from serial_putc_asm clobbering
    ; Print "B" (Before)
    mov al, 'B'
    call serial_putc_asm
    ; Print "1"
    mov al, '1'
    call serial_putc_asm

    pop eax             ; Restore EAX (holds idt_ptr_addr)
    ; --- END DEBUG PRINT (BEFORE LIDT) ---


    ; Original instruction: Load the IDT register (IDTR) using the pointer in EAX
    lidt [eax]


    ; --- BEGIN DEBUG PRINT (AFTER LIDT) ---
    push eax            ; Save EAX (just in case serial_putc_asm clobbers it)

    ; Print "A" (After)
    mov al, 'A'
    call serial_putc_asm
    ; Print "2"
    mov al, '2'
    call serial_putc_asm

    pop eax             ; Restore EAX
    ; --- END DEBUG PRINT (AFTER LIDT) ---


    ret                 ; Return to the C caller (idt_init)