    ; isr_pf.asm - Page Fault ISR Stub (#14) (FIXED)

    section .text
    global isr14            ; Export symbol for IDT registration
    extern page_fault_handler ; C handler function

    ; Page Fault Handler Stub (Interrupt 14)
    ; CPU pushes error code automatically.
    ; We push interrupt number manually.

    isr14:
        ; *** FIX: Push interrupt number FIRST ***
        push dword 14       ; Push interrupt number

        ; 1. Save general registers
        pusha

        ; 2. Save segment registers (DS, ES, FS, GS)
        push ds
        push es
        push fs
        push gs

        ; 3. Pass pointer to saved registers to C handler
        mov eax, esp
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

        ; *** FIX: Pop BOTH interrupt number AND error code ***
        add esp, 8          ; Discard interrupt number (pushed by us)
                            ; AND error code (pushed by CPU)

        ; 7. Return from interrupt
        iret
    