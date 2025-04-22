section .text
global syscall_handler_asm   ; Export symbol for IDT registration
extern syscall_dispatcher    ; C dispatcher function
extern serial_putc_asm     ; External ASM function for serial output (optional debug)

; Define kernel data segment selector (must match your GDT)
%define KERNEL_DATA_SELECTOR 0x10 ; Example value - USE YOUR ACTUAL KERNEL DS

syscall_handler_asm:
    ; CPU pushes: EFLAGS, CS (user), EIP (user)
    ; [SS (user), ESP (user) if privilege change from user to kernel]
    ; Stack top -> bottom: [UserSS], [UserESP], EIP, CS, EFLAGS
    ; NO Error Code is pushed for 'int n'.
    ; EAX contains the syscall number.

    ; --- Optional DEBUG: Print 'S' ---
    ; pusha
    ; mov al, 'S'
    ; call serial_putc_asm
    ; popa
    ; --- End DEBUG ---

    ; 1. Save Segment Registers (that might be used by kernel)
    ;    We save DS, ES, FS, GS. CS/SS are handled by iret.
    push ds
    push es
    push fs
    push gs

    ; 2. Save General Purpose Registers needed by C handler or potentially clobbered
    ;    Save in an order that matches the syscall_regs_t struct *in reverse*
    ;    The C handler expects a pointer to syscall_regs_t.
    ;    The stack pointer (ESP) *before* these pushes will become the base
    ;    pointer to the structure.
    ;    EAX holds the syscall number, save it last before args.
    ;    Syscall args convention (Linux-like): EBX, ECX, EDX, ESI, EDI, EBP
    push eax  ; Save original EAX (syscall number) - will hold return value later
    push ebx  ; Save 1st arg
    push ecx  ; Save 2nd arg
    push edx  ; Save 3rd arg
    push esi  ; Save 4th arg
    push edi  ; Save 5th arg
    push ebp  ; Save 6th arg / Base Pointer

    ; ESP now points to the start of the saved registers (ebp pushed last)
    ; which corresponds to the layout of syscall_regs_t if read from low to high addr.

    ; 3. Set up Kernel Data Segments
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax  ; Or load appropriate FS/GS if using them (e.g., for per-cpu data)
    mov gs, ax

    ; 4. Call the C syscall dispatcher
    push esp                ; Push pointer to the syscall_regs_t structure on the stack
    call syscall_dispatcher ; Call the C function
    add esp, 4              ; Clean up the argument (pointer) from the stack

    ; EAX now holds the return value from the C handler.
    ; We need to place this return value into the saved EAX slot on the stack.
    ; The saved registers are still on the stack below the current ESP.
    ; ESP points just above the saved EBP.
    ; Stack layout: [saved EBP][saved EDI]...[saved EBX][saved EAX][saved GS]...
    ; Offset to saved EAX slot: 6 registers * 4 bytes = 24 bytes
    mov [esp + 24], eax     ; Store return value in the stack slot for the original EAX

    ; 5. Restore General Purpose Registers (except EAX - popa would overwrite return value)
    ;    Pop in reverse order of pushes.
    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ; Don't pop EAX here, its stack slot now holds the return value for the user

    ; 6. Restore Segment Registers (in reverse order)
    pop gs
    pop fs
    pop es
    pop ds

    ; 7. Pop the original EAX value (containing syscall number) into EAX.
    ;    This EAX value *must* be restored correctly because iret expects the
    ;    stack to be exactly as it was after the initial CPU pushes.
    ;    Crucially, the return value for the user program is placed in EAX
    ;    *by the iret instruction itself* using the value we previously stored
    ;    in the saved EAX slot on the stack.
    pop eax                 ; Restore original EAX (syscall number)

    ; 8. Return to user space
    ;    iret will pop EIP, CS, EFLAGS, [UserESP, UserSS] from the stack
    ;    and restore them. The general purpose registers are already restored,
    ;    and the return value is correctly placed in the stack frame for iret
    ;    to load into EAX upon return.
    iret