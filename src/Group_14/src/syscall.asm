; syscall.asm - System Call Handler Assembly Stub
; MODIFIED WITH EXTRA DEBUG PRINTS, INCLUDING ENTRY CHECK

section .text
global syscall_handler_asm    ; Export symbol for IDT registration
extern syscall_dispatcher     ; C dispatcher function
extern serial_putc_asm        ; External ASM function for serial output

; Define kernel data segment selector (must match your GDT)
%define KERNEL_DATA_SELECTOR 0x10 ; Example value - USE YOUR ACTUAL KERNEL DS

; Reserve space for a single global variable to store the syscall number
section .data
orig_syscall_num:   dd 0     ; Storage for original syscall number

section .text
syscall_handler_asm:
    ; --- IMPORTANT: Save syscall number (EAX) to our global variable ---
    ; This is safer than complex stack offset calculations
    mov [orig_syscall_num], eax  ; Save syscall number in our global variable

    ; --- DEBUG: Check AL immediately on entry ---
    push edx                ; Save EDX (used by serial_putc_asm)
    push eax                ; Save original EAX (including AL)

    ; Print '$' marker
    mov al, '$'
    call serial_putc_asm

    ; Print original AL value (high nibble)
    mov dl, [esp + 0]       ; Get low byte (original AL) from saved EAX on stack into DL
    shr dl, 4               ; Get high nibble
    cmp dl, 9
    jle .L_hex1_digit
    add dl, 'A'-10          ; Convert to A-F
    jmp .L_hex1_done
.L_hex1_digit:
    add dl, '0'             ; Convert to 0-9
.L_hex1_done:
    mov al, dl              ; Move hex digit to AL for printing
    call serial_putc_asm    ; Print high nibble

    ; Print original AL value (low nibble)
    mov dl, [esp + 0]       ; Get low byte (original AL) from saved EAX again
    and dl, 0x0F            ; Get low nibble
    cmp dl, 9
    jle .L_hex2_digit
    add dl, 'A'-10          ; Convert to A-F
    jmp .L_hex2_done
.L_hex2_digit:
    add dl, '0'             ; Convert to 0-9
.L_hex2_done:
    mov al, dl              ; Move hex digit to AL for printing
    call serial_putc_asm    ; Print low nibble

    pop eax                 ; Restore original EAX
    pop edx                 ; Restore EDX
    ; --- END IMMEDIATE ENTRY DEBUG ---

    ; --- DEBUG: #0 marker ---
    pusha
    mov al, '#'             ; Indicate ASM handler entry
    call serial_putc_asm
    mov al, '0'             ; Stage 0: Entry marker after initial check
    call serial_putc_asm
    popa

    ; 1. Push dummy error code (0) to match isr_frame_t layout
    push dword 0

    ; 2. Push "interrupt" number (0x80 for syscall)
    push dword 0x80

    ; 3. Save segment registers (DS, ES, FS, GS) to match isr_frame_t
    push ds
    push es
    push fs
    push gs

    ; 4. Save all general purpose registers using pusha
    pusha                  ; Pushes EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX

    ; --- DEBUG: #1 marker ---
    pusha
    mov al, '#'
    call serial_putc_asm
    mov al, '1'            ; Stage 1: After pusha
    call serial_putc_asm
    popa

    ; 5. Set up Kernel Data Segments for C handler execution
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 6. Call the C syscall dispatcher
    ; Calculate pointer to the stack frame
    mov ebx, esp           ; EBX points to saved registers from pusha
    add ebx, 32            ; Adjust to point to the initially pushed gs (start of frame)

    ; --- DEBUG: #2 marker ---
    pusha
    mov al, '#'
    call serial_putc_asm
    mov al, '2'            ; Stage 2: Before C call
    call serial_putc_asm
    popa
    
    ; Get the original syscall number from our global variable
    mov eax, [orig_syscall_num]  ; Get the saved syscall number

    ; Pass both the frame pointer AND the syscall number to dispatcher
    push eax               ; Push the original syscall number as the second argument
    push ebx               ; Push the frame pointer as the first argument
    call syscall_dispatcher ; Call the C function with both arguments
    add esp, 8             ; Clean up the arguments (8 bytes) from the stack

    ; --- DEBUG: #3 marker ---
    ; EAX holds return value from C dispatcher
    pusha
    mov al, '#'
    call serial_putc_asm
    mov al, '3'            ; Stage 3: After C call
    call serial_putc_asm
    popa

    ; Place return value into the saved EAX slot on the stack.
    mov [esp + 28], eax    ; Store return value in the stack slot for EAX

    ; --- DEBUG: #4 marker ---
    pusha
    mov al, '#'
    call serial_putc_asm
    mov al, '4'            ; Stage 4: Before popa
    call serial_putc_asm
    popa

    ; 7. Restore General Purpose Registers
    popa

    ; 8. Restore Segment Registers
    pop gs
    pop fs
    pop es
    pop ds

    ; 9. Clean up the vector number and dummy error code
    add esp, 8

    ; --- DEBUG: #5 marker ---
    pusha
    mov al, '#'
    call serial_putc_asm
    mov al, '5'            ; Stage 5: Before iret
    call serial_putc_asm
    popa

    ; 10. Return to user space
    iret