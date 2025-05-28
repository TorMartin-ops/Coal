; tss_flush.asm
; Loads the TSS selector into the Task Register (TR).

global tss_flush

section .text
tss_flush:
    ; 'selector' is at [esp+4] (cdecl first argument).
    mov ax, [esp + 4]   ; ax <- TSS selector
    ltr ax              ; Load TR with TSS selector
    ret
