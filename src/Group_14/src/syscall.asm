; syscall.asm
; Assembly stub for system calls via INT 0x80.
; This stub saves registers, passes a pointer to the syscall context,
; calls the C syscall_handler, and then returns to user mode.

global syscall_handler_asm
extern syscall_handler  ; Ensure that this matches the C symbol exactly

section .text
syscall_handler_asm:
    pusha                      ; Save general-purpose registers.
    push ds
    push es
    push fs
    push gs
    ; At this point, ESP points to our syscall context.
    mov eax, esp             ; EAX = pointer to syscall_context.
    push eax                 ; Pass pointer as argument.
    call syscall_handler     ; Call C syscall handler.
    add esp, 4               ; Clean up argument.
    pop gs
    pop fs
    pop es
    pop ds
    popa                     ; Restore registers.
    iret                     ; Return from interrupt.
