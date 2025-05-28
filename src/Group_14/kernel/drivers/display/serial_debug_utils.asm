; src/serial_debug_utils.asm
; Provides helper function to print a 32-bit hex value via serial.

section .text
bits 32

global serial_print_hex_asm ; Export for use by other modules
extern serial_putc_asm      ; Import the basic char output function from serial_asm.asm

; --- serial_print_hex_asm ---
; Prints the 32-bit value in EAX as an 8-digit hex number to serial port.
; Uses serial_putc_asm (expects char in AL).
; Preserves all general-purpose registers except EAX (used for char output).
; Modifies: EAX, ECX, EDX (implicitly by serial_putc_asm), ESI
serial_print_hex_asm:
    push ebx    ; Save registers that might be clobbered or we use
    push ecx
    push edx
    push esi
    push edi

    mov esi, eax ; Keep a copy of the number to print in ESI
    mov ecx, 8   ; 8 hex digits for a 32-bit number

.print_digit_loop_hex:
    rol esi, 4   ; Rotate left by 4 bits to get the highest nibble into the lowest
    mov eax, esi ; Copy to EAX so we can work with AL
    and al, 0x0F ; Mask to get only the lowest nibble (0-15)

    ; Convert nibble to ASCII hex character
    cmp al, 9
    jle .is_digit_hex_sub
    add al, 'A' - 10 ; For A-F (uppercase hex)
    jmp .do_print_hex_sub
.is_digit_hex_sub:
    add al, '0'      ; For 0-9

.do_print_hex_sub:
    ; serial_putc_asm expects char in AL and clobbers EAX, EDX.
    ; We need to preserve ECX (loop counter) and ESI (remaining number)
    push ecx
    push esi

    call serial_putc_asm ; Print the character in AL

    pop esi
    pop ecx

    loop .print_digit_loop_hex ; Loop ECX times

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx     ; Restore registers
    ret