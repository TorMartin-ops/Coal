section .text
global syscall_handler_asm   ; Export symbol for IDT registration
extern syscall_dispatcher    ; C dispatcher function
extern serial_putc_asm     ; External ASM function for serial output (optional debug)

; Define kernel data segment selector (must match your GDT)
%define KERNEL_DATA_SELECTOR 0x10 ; Example value - USE YOUR ACTUAL KERNEL DS

syscall_handler_asm:
    ; CPU pushes: EFLAGS, CS (user), EIP (user)
    ; [SS (user), ESP (user) if privilege change]
    ; EAX = syscall number

    ; Save GP registers needed by C (match syscall_regs_t reverse order)
    ; We need to save the original EAX last among these.
    push ebx  ; Arg 1
    push ecx  ; Arg 2
    push edx  ; Arg 3
    push esi  ; Arg 4
    push edi  ; Arg 5
    push ebp  ; Arg 6 / Base Pointer
    push eax  ; Original EAX (Syscall Number) - Saved last

    ; Save User DS, ES, FS, GS *only if C handler might need them*, unlikely.
    ; For simplicity, let's assume C doesn't need them directly.

    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Call C dispatcher
    push esp  ; Push pointer to saved regs (syscall_regs_t*)
    call syscall_dispatcher
    add esp, 4 ; Clean arg

    ; EAX now holds the return value from C handler.
    ; Place it into the saved EAX slot on the stack.
    ; ESP points just above saved EAX. Stack: [Saved EAX][Saved EBP]...
    mov [esp], eax  ; Store return value directly into saved EAX slot

    ; Restore GP registers (excluding EAX, its slot holds return val now)
    pop eax   ; Pop the return value *into* EAX (overwrites syscall number)
    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx

    ; NO explicit "pop gs/fs/es/ds" needed here.

    ; iret will pop EIP, CS, EFLAGS, [UserESP, UserSS]
    ; and restore EAX from the value we just placed there.
    ; It implicitly loads user segments based on popped CS/SS.
    iret