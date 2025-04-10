; isr_pf.asm - Page Fault ISR Stub (#14)

section .text
global isr14            ; Export symbol for IDT registration
extern page_fault_handler ; C handler function

; Page Fault Handler Stub (Interrupt 14)
; The CPU automatically pushes an error code for page faults.
; Stack upon entry:
;   [ESP]   -> EIP         (Return address)
;   [ESP+4] -> CS          (Code Segment)
;   [ESP+8] -> EFLAGS
;   [ESP+12]-> ESP_user    (if privilege change)
;   [ESP+16]-> SS_user     (if privilege change)
;   [ESP+??]-> Error Code  <- Pushed by CPU BEFORE our handler runs!
;
; We need to:
; 1. Push our own placeholder for the interrupt number (14).
; 2. Push general registers (pusha).
; 3. Push segment registers.
; 4. Call the C handler, passing pointer to the saved state.
; 5. Pop segment registers.
; 6. Pop general registers (popa).
; 7. Discard the error code AND our placeholder interrupt number.
; 8. Return using iret.

isr14:
    ; 1. Save general registers
    pusha

    ; 2. Save segment registers (DS, ES, FS, GS)
    push ds
    push es
    push fs
    push gs

    ; 3. Pass pointer to saved registers (including error code & iret frame) to C handler
    mov eax, esp        ; EAX points to the start of saved segments (which is start of our 'registers_t' view)
    push eax            ; Push argument for page_fault_handler
    call page_fault_handler
    add esp, 4          ; Clean up argument from stack

    ; 4. Restore segment registers
    pop gs
    pop fs
    pop es
    pop ds

    ; 5. Restore general registers
    popa

    ; 6. Discard the error code pushed by the CPU
    add esp, 4          ; Remove error code from stack

    ; 7. Return from interrupt
    iret