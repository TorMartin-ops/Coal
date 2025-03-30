; tss_flush.asm
; Assembly routine to load the Task State Segment (TSS).
; It takes the TSS selector as an argument.
global tss_flush
section .text
tss_flush:
    mov ax, [esp+4]  ; Get TSS selector from stack.
    ltr ax           ; Load TR register with TSS selector.
    ret
