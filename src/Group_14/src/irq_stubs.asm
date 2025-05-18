; =============================================================================
;  src/irq_stubs.asm — IRQ 0-15 handler stubs (vectors 32-47)
;  ----------------------------------------------------------
;  • Unified prologue/epilogue for every IRQ.
;  • Correct stack-frame layout for C side (isr_frame_t).
;  • Optional one-byte debug marker per entry/exit (serial @115200 8N1).
;  • Zero dynamic symbols: everything resolved at link-time.
;  ---------------------------------------------------------------------------
;  Build-time feature flags
;    DEBUG_IRQ_STUBS  – emit ‘@’ on entry / ‘#’ on exit of common stub
;                       (define in NASM command line: -DDEBUG_IRQ_STUBS=1)
;    DEBUG_IRQ1_DIRECT – emit '1' directly from irq1 stub
;                       (define in NASM command line: -DDEBUG_IRQ1_DIRECT=1)
; =============================================================================

        bits    32
        default rel                     ; safer for PIE kernels (no effect on flat)

; -----------------------------------------------------------------------------
; External C symbols
; -----------------------------------------------------------------------------
        extern  isr_common_handler      ; void isr_common_handler(isr_frame_t*)
; serial_putc_asm will be used if any debug flag is set
%ifidn __OUTPUT_FORMAT__, elf32
    %ifdef DEBUG_IRQ_STUBS
        extern  serial_putc_asm         ; void serial_putc_asm(uint8_t)
    %endif
    %ifdef DEBUG_IRQ1_DIRECT
        %ifndef DEBUG_IRQ_STUBS         ; Declare if not already declared
            extern  serial_putc_asm
        %endif
    %endif
%endif


; -----------------------------------------------------------------------------
; Segments & constants
; -----------------------------------------------------------------------------
KERNEL_DS       equ     0x10            ; must match your GDT
IRQ_BASE_VEC    equ     32              ; PIC remap base (0x20)

; -----------------------------------------------------------------------------
; Public IRQ labels (used by idt.c)
; -----------------------------------------------------------------------------
%assign i 0
%rep 16
        global  irq %+ i
%assign i i+1
%endrep

; -----------------------------------------------------------------------------
; Macro: DECL_IRQ <n>
; Generates:
;   irq<n> stub        – pushes fake error-code & vector, jumps to common.
; -----------------------------------------------------------------------------
%macro DECL_IRQ_GENERAL 1
irq%1:                                  ; label visible to linker
    push    dword 0                 ; dummy error code
    push    dword IRQ_BASE_VEC+%1   ; vector number
    jmp     irq_common_stub
%endmacro

; --- Special handler for IRQ1 (Keyboard) with direct debug output ---
irq1:
%ifdef DEBUG_IRQ1_DIRECT
    pusha                           ; Save all general purpose registers
    mov     al, '1'                 ; Character '1' to signify IRQ1 was hit
    call    serial_putc_asm         ; Call external ASM routine to print char in AL
    popa                            ; Restore all general purpose registers
%endif
    push    dword 0                 ; dummy error code for IRQ1
    push    dword IRQ_BASE_VEC+1    ; vector number for IRQ1 (33)
    jmp     irq_common_stub

; --- Generate other IRQ stubs ---
DECL_IRQ_GENERAL 0  ; IRQ0 (PIT)

; IRQs 2 through 15
%assign i 2
%rep 14                             ; 16 total IRQs, 0 and 1 handled, so 14 left
    DECL_IRQ_GENERAL i
%assign i i+1
%endrep


; -----------------------------------------------------------------------------
; Common stub – builds isr_frame_t, calls C, restores context, IRET
; -----------------------------------------------------------------------------
irq_common_stub:

%ifdef DEBUG_IRQ_STUBS
        pusha
        mov     al, '@'
        call    serial_putc_asm
        popa
%endif

        push    ds
        push    es
        push    fs
        push    gs
        pusha                           
        mov     ax, KERNEL_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, esp
        push    eax
        call    isr_common_handler
        add     esp, 4                  
        popa
        pop     gs
        pop     fs
        pop     es
        pop     ds
        add     esp, 8

%ifdef DEBUG_IRQ_STUBS
        pusha
        mov     al, '#'
        call    serial_putc_asm
        popa
%endif
        iret