; src/syscall.asm - System Call Handler Stub (e.g., for int 0x80)

section .text
global syscall_handler_asm  ; Export symbol for IDT registration (if using INT)
extern syscall_handler      ; C handler function
extern serial_putc_asm  ; External ASM function for serial output

; Define kernel data segment selector (must match your GDT)
%define KERNEL_DATA_SELECTOR 0x10

syscall_handler_asm:
    ; --- DEBUG: Print 'S' for Syscall Entry ---
    pusha               ; Save regs temporarily
    mov al, 'S'
    call serial_putc_asm
    popa                ; Restore regs
    ; --- End DEBUG ---

    ; CPU pushes: EFLAGS, CS (user), EIP (user), [SS (user), ESP (user) if priv change]
    ; Stack top -> bottom: [UserSS], [UserESP], EIP, CS, EFLAGS
    ; NO Error Code is pushed for 'int n'.

    ; 1. Save general registers using pusha (ensure order matches syscall_context_t)
    pusha               ; edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax (Syscall number is in EAX)

    ; 2. Save segment registers used by kernel (DS, ES important, FS/GS maybe too)
    push ds
    push es
    push fs
    push gs

    ; 3. Set up Kernel Data Segments
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    ; mov fs, ax ; Example if needed
    ; mov gs, ax ; Example if needed

    ; 4. Call the C syscall handler
    push esp            ; Push pointer to context structure
    call syscall_handler
    add esp, 4          ; Clean up argument from stack

    ; EAX now holds the return value from the C handler.

    ; 5. Restore segment registers (in reverse order)
    pop gs
    pop fs
    pop es
    pop ds

    ; 6. Restore general registers (EAX contains the return value, popa restores it)
    popa

    ; 7. Return to user space
    iret