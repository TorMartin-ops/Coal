; ===============================
;  IRQ STUBS (irq_stubs.asm)
;  -------------------------------
;  Fixed, annotated version that builds a correct isr_frame_t for C,
;  adds optional serial‑debug markers, and gives IRQ 1 a dedicated stub
;  (needed for early keyboard bring‑up).
; ===============================
bits 32
default rel

; --------------------------------------------------------------------------
; External C / ASM helpers
; --------------------------------------------------------------------------
extern  isr_common_handler      ; void isr_common_handler(isr_frame_t*)

; serial_putc_asm will be used if any debug flag is set
; Ensure DEBUG_IRQ_STUBS and DEBUG_IRQ1_DIRECT are defined in your Makefile/CMake if needed
%ifdef DEBUG_IRQ_STUBS
    extern  serial_putc_asm     ; serial debug helper (8‑N‑1 @ 115200)
%endif
%ifdef DEBUG_IRQ1_DIRECT
    %ifndef DEBUG_IRQ_STUBS         ; Declare if not already declared
        extern  serial_putc_asm
    %endif
%endif

; --------------------------------------------------------------------------
; Segments & constants
; --------------------------------------------------------------------------
KERNEL_DS       equ     0x10            ; must match your GDT data‑segment
IRQ_BASE_VEC    equ     32              ; PIC remap base (0x20)

; --------------------------------------------------------------------------
; Public IRQ labels (used by idt.c)
; --------------------------------------------------------------------------
%assign i 0
%rep 16
    global  irq %+ i
%assign i i+1
%endrep

; --------------------------------------------------------------------------
; Helper macro for general IRQs (excluding IRQ1 which has a special stub)
; --------------------------------------------------------------------------
%macro DECL_IRQ_GENERAL 1
irq%1:
    push    dword 0                 ; fake error‑code (hardware IRQs don’t push one)
    push    dword IRQ_BASE_VEC+%1   ; vector #
    jmp     irq_common_stub
%endmacro

; --- IRQ0 (PIT Timer) ---
DECL_IRQ_GENERAL 0

; --- IRQ1 (Keyboard) – optional direct serial trace ---
irq1:
%ifdef DEBUG_IRQ1_DIRECT
    pusha
    mov     al, '1'                 ; Character '1' to signify IRQ1 was hit
    call    serial_putc_asm         ; Call external ASM routine to print char in AL
    popa
%endif
    push    dword 0                 ; dummy error code for IRQ1
    push    dword IRQ_BASE_VEC+1    ; vector number for IRQ1 (33)
    jmp     irq_common_stub

; --- IRQ2‑IRQ15 ---
%assign i 2
%rep 14                             ; IRQs 2 through 15
    DECL_IRQ_GENERAL i
%assign i i+1
%endrep

; --------------------------------------------------------------------------
; Common stub for IRQs – builds stack frame & jumps to C
; --------------------------------------------------------------------------
irq_common_stub:
%ifdef DEBUG_IRQ_STUBS
    pusha
    mov     al, '@'                 ; entry marker for IRQ common stub
    call    serial_putc_asm
    popa
%endif
    push    ds                      ; Save segment registers
    push    es
    push    fs
    push    gs
    pusha                           ; Save all general purpose registers (EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX)

    mov     ax, KERNEL_DS           ; Load kernel data segment selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    mov     eax, esp                ; ESP now points to the top of the saved registers (start of isr_frame_t)
    push    eax                     ; Pass pointer to isr_frame_t as argument
    call    isr_common_handler      ; Call the C common handler
    add     esp, 4                  ; Clean up argument from stack

    popa                            ; Restore general purpose registers
    pop     gs                      ; Restore segment registers
    pop     fs
    pop     es
    pop     ds

    add     esp, 8                  ; Pop int‑no (vector #) + fake error‑code

%ifdef DEBUG_IRQ_STUBS
    pusha
    mov     al, '#'                 ; exit marker for IRQ common stub
    call    serial_putc_asm
    popa
%endif
    iret                            ; Return from interrupt
; ===============================
; END OF IRQ STUBS
; ===============================