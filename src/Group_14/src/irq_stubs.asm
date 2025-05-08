; =============================================================================
;  src/irq_stubs.asm — IRQ 0-15 handler stubs (vectors 32-47)
;  ----------------------------------------------------------
;  • Unified prologue/epilogue for every IRQ.
;  • Correct stack-frame layout for C side (isr_frame_t).
;  • Optional one-byte debug marker per entry/exit (serial @115 200 8N1).
;  • Zero dynamic symbols: everything resolved at link-time.
;  ---------------------------------------------------------------------------
;  Build-time feature flags
;     DEBUG_IRQ_STUBS  – emit ‘@’ on entry / ‘#’ on exit of common stub
;                        (define in NASM command line: -DDEBUG_IRQ_STUBS=1)
; =============================================================================

        bits    32
        default rel               ; safer for PIE kernels (no effect on flat)

; -----------------------------------------------------------------------------
; External C symbols
; -----------------------------------------------------------------------------
        extern  isr_common_handler        ; void isr_common_handler(isr_frame_t*)
%ifdef DEBUG_IRQ_STUBS
        extern  serial_putc_asm           ; void serial_putc_asm(uint8_t)
%endif

; -----------------------------------------------------------------------------
; Segments & constants
; -----------------------------------------------------------------------------
KERNEL_DS       equ     0x10              ; must match your GDT
IRQ_BASE_VEC    equ     32               ; PIC remap base (0x20)

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
;   irq<n> stub         – pushes fake error-code & vector, jumps to common.
; -----------------------------------------------------------------------------
%macro DECL_IRQ 1
irq%1:                                  ; label visible to linker
        push    dword 0                 ; dummy error code
        push    dword IRQ_BASE_VEC+%1   ; vector number
        jmp     irq_common_stub
%endmacro

; Generate IRQ0-IRQ15
%assign i 0
%rep 16
        DECL_IRQ i
%assign i i+1
%endrep

; -----------------------------------------------------------------------------
; Common stub – builds isr_frame_t, calls C, restores context, IRET
; -----------------------------------------------------------------------------
irq_common_stub:

%ifdef DEBUG_IRQ_STUBS
        ; One-byte marker so you can scope-grep in serial log.
        pusha
        mov     al, '@'
        call    serial_putc_asm
        popa
%endif

        ; ---- Save segment registers ----
        push    ds
        push    es
        push    fs
        push    gs

        ; ---- Save GP registers ----
        pusha                           ; EDI,ESI,EBP,ESP*,EBX,EDX,ECX,EAX

        ; ---- Switch to kernel data segments ----
        mov     ax, KERNEL_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax

        ; ---- Call into C (argument = current ESP) ----
        mov     eax, esp
        push    eax
        call    isr_common_handler
        add     esp, 4                 ; pop arg

        ; ---- Restore GP registers ----
        popa

        ; ---- Restore original segment registers ----
        pop     gs
        pop     fs
        pop     es
        pop     ds

        ; ---- Discard vector & fake error code pushed earlier ----
        add     esp, 8

%ifdef DEBUG_IRQ_STUBS
        pusha
        mov     al, '#'
        call    serial_putc_asm
        popa
%endif

        iret
