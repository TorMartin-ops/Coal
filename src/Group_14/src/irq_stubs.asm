; Corrected src/irq_stubs.asm
; Defines Interrupt Request (IRQ) handler stubs for IRQs 0-15 (vectors 32-47).
; Uses a common stub to ensure correct stack frame for the C handler.

section .text

; External C handler function
extern isr_common_handler ; Make sure C code defines this function

; Export IRQ symbols for use in IDT setup
global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

; Define Kernel Data Segment selector (adjust if different in gdt.c)
KERNEL_DATA_SEG equ 0x10

; Macro for defining IRQ handlers.
; IRQs do not push an error code automatically, so we push 0.
%macro IRQ_HANDLER 1
irq%1:
    ; cli            ; Optional: Disable interrupts on entry if not using Interrupt Gates
    push dword 0   ; Push dummy error code for IRQ
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
; Assumes KERNEL_DATA_SEG is defined above.
common_interrupt_stub:
    ; CPU/Stub already pushed ErrorCode and VectorNumber
    pusha          ; Save GP registers

    ; Save the original DS, ES, FS, GS *temporarily* if needed by C
    ; but DO NOT pop them back just before IRET. IRET handles segments.
    ; For simplicity, if C handler doesn't need them, skip push/pop.
    ; Let's assume C doesn't need them for now.

    mov ax, KERNEL_DATA_SEG ; Kernel Data Selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov eax, esp    ; Get pointer to the stack frame base (after pusha)
                    ; Adjust isr_frame_t definition if DS/ES/FS/GS aren't pushed
    push eax        ; Push argument for C handler
    call isr_common_handler
    add esp, 4      ; Clean up argument

    ; NO explicit "pop gs/fs/es/ds" here

    popa            ; Restore GP registers

    add esp, 8      ; Clean up Vector Number and Error Code

    iret            ; Return, restores EIP, CS, EFLAGS, ESP, SS
                    ; implicitly loads DS, ES, FS, GS based on new SS/CS