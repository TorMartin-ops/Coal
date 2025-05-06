; -----------------------------------------------------------------------------
; syscall.asm  – INT 0x80 entry/exit stub for UiAOS (v5.4 - Corrected EAX Debug & Return)
; -----------------------------------------------------------------------------
%define KERNEL_DATA_SELECTOR 0x10
%define USER_DATA_SELECTOR   0x23

    extern syscall_dispatcher
    extern serial_putc_asm
    extern serial_print_hex_asm

    section .text
    global syscall_handler_asm

syscall_handler_asm:
    ; --- 1) Push error code (dummy) and int_no ---
    push dword 0            ; err_code = 0
    push dword 0x80         ; int_no   = 0x80

    ; --- 2) Save user segments ---
    push ds
    push es
    push fs
    push gs

    ; --- 3) Push general-purpose registers ---
    pusha   ; Pushes: EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX

    ; (Optional) Simple trace: print '<' to indicate entry
    ; This call must preserve EAX if it's to be used immediately after for something else,
    ; or ensure EAX is reloaded if necessary. Here, EAX from PUSHA is fine to be clobbered.
    mov al, '<'
    call serial_putc_asm

    ; --- 4) Switch to kernel data segments ---
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- 5) Call C dispatcher ---
    ; ESP currently points to the start of the PUSHA frame (saved EDI).
    ; This is the base of our isr_frame_t.
    mov eax, esp            ; Pass &isr_frame_t (current ESP) as argument
    push eax                ; Push argument onto stack
    call syscall_dispatcher   ; syscall_dispatcher will return its result in EAX
    add  esp, 4             ; Clean up argument from stack

    ; At this point, EAX holds the true return value from syscall_dispatcher.

    ; --- (Optional) Trace the return value from C dispatcher ---
    ; To safely print EAX, we must preserve it if serial_print_hex_asm modifies EAX.
    push eax                  ; Save the true return value
    
    ; Now print the saved value. We can use EBX as a temporary for the value to print.
    mov ebx, [esp + 0]        ; Get the pushed EAX into EBX
    
    pushad                    ; Save all registers for the serial printing block
    mov al, '['
    call serial_putc_asm
    mov al, 'R'               ; Tag for "Return from C"
    call serial_putc_asm
    mov al, ':'
    call serial_putc_asm
    mov eax, ebx              ; Load value to print into EAX (for serial_print_hex_asm)
    call serial_print_hex_asm
    mov al, ']'
    call serial_putc_asm
    mov al, ' '
    call serial_putc_asm
    popad                     ; Restore registers clobbered by this print block

    pop eax                   ; Restore the true return value into EAX

    ; --- 6) Write the true return value (now in EAX) back into the saved EAX slot in PUSHA frame ---
    ; The PUSHA frame is on the stack. ESP points to the saved EDI.
    ; Offset of saved EAX within PUSHA frame (when ESP points to EDI):
    ; EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX
    ;  0    4    8      12      16   20   24   28
    mov [esp + 28], eax

    ; --- 7) Restore registers and segments ---
    popa    ; EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX (EAX now has the syscall result)
    
    ; (Optional) Log EAX after popa to confirm it's correct
    pushad
    mov ebx, eax ; Save EAX
    mov al, '['
    call serial_putc_asm
    mov al, 'L' ; Loaded EAX
    call serial_putc_asm
    mov al, ':'
    call serial_putc_asm
    mov eax, ebx
    call serial_print_hex_asm
    mov al, ']'
    call serial_putc_asm
    mov al, ' '
    call serial_putc_asm
    popad

    pop  gs
    pop  fs
    pop  es
    pop  ds

    add  esp, 8             ; Remove int_no, err_code from stack

    ; (Optional) Trace '>' for exit
    mov al, '>'
    call serial_putc_asm

    ; --- 8) Return to user mode ---
    iret
