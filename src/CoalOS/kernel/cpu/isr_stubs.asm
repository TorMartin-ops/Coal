; ===============================
;  EXCEPTION STUBS 0‑19 (isr_stubs.asm)
;  -------------------------------
;  Uses common_interrupt_stub; ISR14 removed.
; ===============================
section .text
bits 32
default rel

; External C / ASM helpers
extern isr_common_handler      ; void isr_common_handler(isr_frame_t*)

; Define DEBUG_ISR_STUBS in build flags if serial debug markers are needed for exceptions
%ifdef DEBUG_ISR_STUBS
    extern serial_putc_asm     ; serial debug helper
%endif

; Publicly exported labels for IDT setup
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8
global isr10, isr11, isr12, isr13, isr16, isr17, isr18, isr19
; ISR9 and ISR15 are often reserved or specific, add if needed.

KERNEL_DS equ 0x10 ; Must match your GDT data-segment selector

; Common macro for ISRs WITHOUT an error code pushed by CPU
; We push a dummy error code 0.
%macro ISR_NOERR 1
isr%1:
    ; cli ; Interrupts are usually disabled by CPU on exception entry
    push dword 0    ; Push dummy error code
    push dword %1   ; Push vector number (interrupt number)
    jmp  common_interrupt_stub
%endmacro

; Common macro for ISRs WITH an error code pushed by the CPU
%macro ISR_ERR 1
isr%1:
    ; cli
    ; Error code is already pushed by CPU
    push dword %1   ; Push vector number (interrupt number)
    jmp  common_interrupt_stub
%endmacro

; Define ISRs using the macros
ISR_NOERR  0    ; Divide By Zero Exception
ISR_NOERR  1    ; Debug Exception
ISR_NOERR  2    ; Non Maskable Interrupt Exception
ISR_NOERR  3    ; Breakpoint Exception
ISR_NOERR  4    ; Into Detected Overflow Exception
ISR_NOERR  5    ; Out of Bounds Exception
ISR_NOERR  6    ; Invalid Opcode Exception
ISR_NOERR  7    ; No Coprocessor Exception / Device Not Available
ISR_ERR    8    ; Double Fault (pushes error code)
; ISR 9 - Coprocessor Segment Overrun (typically not used on modern systems)
ISR_ERR   10    ; Invalid TSS Exception (pushes error code)
ISR_ERR   11    ; Segment Not Present Exception (pushes error code)
ISR_ERR   12    ; Stack Fault Exception (pushes error code)

; ISR 13 - General Protection Fault (pushes error code) - with extra debug
isr13:
%ifdef DEBUG_ISR_STUBS ; Or a more specific DEBUG_GP_FAULT
    pusha
    mov     al, 'G'
    call    serial_putc_asm
    mov     al, 'P'
    call    serial_putc_asm
    mov     al, '!'
    call    serial_putc_asm
    popa
%endif
    ; Error code is already pushed by CPU
    push    dword 13 ; Push vector number
    jmp     common_interrupt_stub

; ISR 14 (Page Fault) is handled by isr_pf.asm, so it's NOT defined here.

; ISR 15 - Reserved by Intel.

ISR_NOERR 16    ; Floating Point Exception (x87 FPU)
ISR_ERR   17    ; Alignment Check Exception (pushes error code)
ISR_ERR   18    ; Machine Check Exception (pushes error code - MCE is complex)
ISR_NOERR 19    ; SIMD Floating-Point Exception (SSE)

; Exceptions 20-31 are reserved by Intel.

; --------------------------------------------------------------------------
; Common stub for Exceptions – builds stack frame & jumps to C
; This stub is shared between exception ISRs (0-19, excluding 14)
; --------------------------------------------------------------------------
common_interrupt_stub:
%ifdef DEBUG_ISR_STUBS
    pusha
    mov     al, 'X'                 ; Entry marker for Exception common stub (to differentiate from IRQ '@')
    call    serial_putc_asm
    popa
%endif
    push    ds                      ; Save segment registers
    push    es
    push    fs
    push    gs
    pusha                           ; Save all general purpose registers

    mov     ax, KERNEL_DS           ; Load kernel data segment selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    mov     eax, esp                ; ESP now points to the start of isr_frame_t
    push    eax                     ; Pass pointer to isr_frame_t as argument
    call    isr_common_handler      ; Call the C common handler
    add     esp, 4                  ; Clean up argument from stack

    popa                            ; Restore general purpose registers
    pop     gs                      ; Restore segment registers
    pop     fs
    pop     es
    pop     ds

    add     esp, 8                  ; Pop int_no (vector #) + error_code (real or dummy)

    ; No sti here, iret will restore EFLAGS which includes the original interrupt flag state
    iret                            ; Return from interrupt
; ===============================
; END OF EXCEPTION STUBS
; ===============================