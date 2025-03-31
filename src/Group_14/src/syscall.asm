; syscall.asm
; Assembly stub for system calls via INT 0x80.

global syscall_handler_asm
extern syscall_handler  ; The C-level function we call.

section .text

; 
; syscall_handler_asm - Interrupt Service Routine for INT 0x80.
; Saves CPU registers and segments, sets DS/ES/FS/GS to known values,
; then calls C function `syscall_handler(syscall_context_t *ctx)`.
; The stack layout at entry (x86 cdecl):
;   [ESP] = EIP to return from
;   [ESP+4..] saved by CPU for iret, but we push them in pushad, etc.
;

syscall_handler_asm:
    pusha                      ; Save general-purpose registers (EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX).
    push ds
    push es
    push fs
    push gs

    ; (Optional) Set known data segments if you use ring 3 or want consistent segments.
    mov ax, 0x10              ; 0x10 is the kernel data selector (index=2 in GDT).
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; At this point, ESP points to the top of the "syscall context" structure:
    ;   struct syscall_context_t + saved EIP, code segment, EFLAGS, ...
    ; We'll pass the pointer to the structure to our C handler.
    mov eax, esp              ; EAX = &saved registers
    push eax                  ; push as an argument to syscall_handler(...)

    call syscall_handler      ; EAX = return value from C (optionally stored in ctx->eax).
    add esp, 4                ; pop the argument from stack

    pop gs
    pop fs
    pop es
    pop ds
    popa                      ; Restore general-purpose registers
    iret                      ; Return from interrupt => resume caller
