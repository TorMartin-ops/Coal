; Corrected src/irq_stubs.asm
; Defines Interrupt Request (IRQ) handler stubs for IRQs 0-15 (vectors 32-47).
; Uses a common stub to ensure correct stack frame for the C handler.
; <<< MODIFIED: Added debug print to common_interrupt_stub >>>

section .text

; External C handler function
extern isr_common_handler ; Make sure C code defines this function
extern serial_putc_asm    ; External ASM function for serial output

; Export IRQ symbols for use in IDT setup
global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

; Define Kernel Data Segment selector (adjust if different in gdt.c)
KERNEL_DATA_SEG equ 0x10

; Macro for defining IRQ handlers.
; IRQs do not push an error code automatically, so we push 0.
%macro IRQ_HANDLER 1
irq%1:
    ; cli              ; Optional: Disable interrupts on entry if not using Interrupt Gates
    push dword 0     ; Push dummy error code for IRQ
    push dword 32 + %1 ; Push vector number (IRQ 0 = 32, etc.)
    jmp common_interrupt_stub ; Jump to common code
%endmacro

; Define IRQ handlers 0 through 15 using the macro
IRQ_HANDLER 0   ; Timer (Vector 32)
IRQ_HANDLER 1   ; Keyboard (Vector 33)
IRQ_HANDLER 2   ; Cascade (Vector 34)
IRQ_HANDLER 3   ; COM2 (Vector 35)
IRQ_HANDLER 4   ; COM1 (Vector 36)
IRQ_HANDLER 5   ; LPT2 (Vector 37)
IRQ_HANDLER 6   ; Floppy Disk (Vector 38)
IRQ_HANDLER 7   ; LPT1 / Spurious (Vector 39)
IRQ_HANDLER 8   ; RTC (Vector 40)
IRQ_HANDLER 9   ; Free / ACPI SCI (Vector 41)
IRQ_HANDLER 10  ; Free (Vector 42)
IRQ_HANDLER 11  ; Free (Vector 43)
IRQ_HANDLER 12  ; PS/2 Mouse (Vector 44)
IRQ_HANDLER 13  ; FPU Coprocessor (Vector 45)
IRQ_HANDLER 14  ; Primary ATA Hard Disk (Vector 46)
IRQ_HANDLER 15  ; Secondary ATA Hard Disk (Vector 47)


; Common stub called by all ISRs and IRQs after pushing vector and error code.
; Creates the stack frame expected by the C isr_common_handler(isr_frame_t* frame).
common_interrupt_stub:
    ; --- DEBUG PRINT: Indicate entry into common stub ---
    pusha           ; Save registers temporarily
    mov al, '@'     ; '@' signifies entry to common stub
    call serial_putc_asm
    popa            ; Restore registers
    ; --- END DEBUG PRINT ---

    ; 1. Save segment registers (DS, ES, FS, GS) first, as isr_frame_t expects them
    push ds
    push es
    push fs
    push gs

    ; 2. Save all general purpose registers
    pusha          ; Pushes EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX

    ; 3. Load kernel data segments into segment registers for C handler execution
    mov ax, KERNEL_DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax     ; GS is now set (original was saved by initial push gs)

    ; 4. Call the C handler, passing a pointer to the saved state.
    mov eax, esp   ; Get pointer to the start of the isr_frame_t structure
    push eax       ; Push pointer as argument for isr_common_handler
    call isr_common_handler
    add esp, 4     ; Clean up argument stack

    ; 5. Restore segment registers (placeholder pop gs, real restore is later)
    ;    pop gs <-- REMOVED (See comment in isr_stubs.asm)

    ; 6. Restore general purpose registers
    popa

    ; 7. Restore original segment registers (popped in reverse order of push)
    pop gs ; <<< CORRECTED: Restore original GS here
    pop fs
    pop es
    pop ds

    ; 8. Clean up the vector number and error code pushed by the specific stub
    add esp, 8

    ; 9. Return from interrupt
    ; sti
    iret