    section .text
    global syscall_handler_asm    ; Export symbol for IDT registration
    extern syscall_dispatcher     ; C dispatcher function
    extern serial_putc_asm      ; External ASM function for serial output (optional debug)

    ; Define kernel data segment selector (must match your GDT)
    %define KERNEL_DATA_SELECTOR 0x10 ; Example value - USE YOUR ACTUAL KERNEL DS

    syscall_handler_asm:
        ; CPU pushes: EFLAGS, CS (user), EIP (user)
        ; [SS (user), ESP (user) if privilege change from user to kernel]
        ; Stack top -> bottom: [UserSS], [UserESP], EFLAGS, CS, EIP
        ; NO Error Code is pushed for 'int n'.
        ; EAX contains the syscall number.

        ; --- Optional DEBUG: Print 'S' ---
        ; pusha
        ; mov al, 'S'
        ; call serial_putc_asm
        ; popa
        ; --- End DEBUG ---

        ; 1. Push dummy error code (0) to match isr_frame_t layout
        push dword 0

        ; 2. Push "interrupt" number (0x80 for syscall)
        push dword 0x80

        ; 3. Save segment registers (DS, ES, FS, GS) to match isr_frame_t
        ;    These will hold the USER segments initially.
        push ds
        push es
        push fs
        push gs

        ; 4. Save all general purpose registers using pusha
        pusha          ; Pushes EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX

        ; --- Stack now matches isr_frame_t layout from the pushed gs upwards ---

        ; 5. Set up Kernel Data Segments for C handler execution
        mov ax, KERNEL_DATA_SELECTOR
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax

        ; 6. Call the C syscall dispatcher
        ;    Pass pointer to the base of the saved frame (points to the pushed gs)
        mov eax, esp     ; ESP points to saved EAX from pusha
        add eax, 32      ; Adjust ESP to point to the initially pushed gs (start of frame)
        push eax         ; Push pointer to the syscall_regs_t/isr_frame_t structure
        call syscall_dispatcher ; Call the C function
        add esp, 4       ; Clean up the argument (pointer) from the stack

        ; EAX now holds the return value from the C handler.
        ; We need to place this return value into the saved EAX slot on the stack.
        ; The saved EAX is at [ESP + offset_to_eax_from_pusha]
        ; Offset = EDI,ESI,EBP,ESP_dummy,EBX,EDX,ECX = 7 * 4 = 28 bytes
        mov [esp + 28], eax ; Store return value in the stack slot for EAX

        ; 7. Restore General Purpose Registers (including EAX which now holds return value)
        popa

        ; 8. Restore Segment Registers (popped in reverse order)
        pop gs
        pop fs
        pop es
        pop ds

        ; 9. Clean up the vector number (0x80) and dummy error code (0)
        add esp, 8

        ; 10. Return to user space
        ;     iret will pop EIP, CS, EFLAGS, [UserESP, UserSS] from the stack
        ;     and restore them. The general purpose registers (including EAX
        ;     with the return value) are already restored by popa.
        iret
    