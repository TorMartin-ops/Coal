; src/serial_asm.asm - CORRECTED
; Provides a simple assembly function to write a character to COM1 using polling.
; Intended for very early kernel debugging before interrupts are enabled.

section .text
global serial_putc_asm      ; Export for use by other modules

SERIAL_PORT equ 0x3F8       ; COM1 base port address
SERIAL_LSR  equ SERIAL_PORT + 5 ; Line Status Register (offset 5)

; --- serial_putc_asm ---
; Writes the character in the AL register to the serial port (COM1).
; Waits for the transmit buffer to be empty before writing (polling).
; Inputs:
;   AL = Character to write
; Outputs:
;   None
; Clobbers:
;   EAX, EDX (used for port I/O)
serial_putc_asm:
    ; Preserve registers that might be needed by the caller
    push edx
    push eax            ; Save original EAX (including the char in AL)

.wait_for_transmit_empty:
    mov dx, SERIAL_LSR  ; Point DX to the Line Status Register port
    in al, dx           ; Read the LSR into AL (temporarily overwrites char in AL)
    test al, 0x20       ; Check bit 5 (Transmit Holding Register Empty - THRE)
    jz .wait_for_transmit_empty ; If bit 5 is zero, loop and wait (busy-wait)

    ; The transmit buffer is empty, we can now write the character.
    ; Restore original EAX (which has the character in AL) from the stack
    pop eax

    ; Now AL holds the character we actually want to print
    mov dx, SERIAL_PORT ; Point DX to the Data port
    out dx, al          ; Write the correct character from AL to the port

    ; Restore other clobbered registers and return
    pop edx
    ret                 ; Return to caller
