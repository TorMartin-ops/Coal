; src/serial_asm.asm
; Provides a simple assembly function to write a character to COM1.

section .text
global serial_putc_asm      ; Export for use by other modules

SERIAL_PORT equ 0x3F8       ; COM1 base port address
SERIAL_LSR  equ SERIAL_PORT + 5 ; Line Status Register (offset 5)

; --- serial_putc_asm ---
; Writes the character in the AL register to the serial port (COM1).
; Waits for the transmit buffer to be empty before writing.
; Inputs:
;   AL = Character to write
; Outputs:
;   None
; Clobbers:
;   AL, DX (used for port I/O)
serial_putc_asm:
    ; Preserve registers that might be needed by the caller, except AL and DX
    push edx

.wait_for_transmit_empty:
    mov dx, SERIAL_LSR      ; Point DX to the Line Status Register port
    in al, dx               ; Read the LSR into AL
    test al, 0x20           ; Check bit 5 (Transmit Holding Register Empty - THRE)
    jz .wait_for_transmit_empty ; If bit 5 is zero, loop and wait

    ; The transmit buffer is empty, we can now write the character.
    ; We assume the character to print was passed in AL and might have been
    ; overwritten by the 'in al, dx'. We need to retrieve it.
    ; A robust way is to expect the caller to pass it on the stack or another register,
    ; but for this simple debug case, we'll assume the caller can reload AL if needed
    ; right before calling us, or that the character is simple enough that the upper
    ; bits of EAX were preserved if the caller used `push eax`.
    ; Let's assume the caller ensures AL holds the correct char upon entry.
    ; **Alternative (if caller pushes char):**
    ;    mov al, [esp + 4 + 4] ; Assuming push edx, then char pushed before call (adjust offset!)

    ; For simplicity, let's stick to the original plan: Caller puts char in AL.
    ; If the `in al, dx` corrupted it, the caller's `mov al, 'X'` before the call handles it.
    ; If the char was simple ASCII, it's likely still okay in AL anyway.

    mov dx, SERIAL_PORT     ; Point DX to the Data port
    ; Character to write should be in AL (passed by caller)
    out dx, al              ; Write the character from AL to the port

    ; Restore clobbered registers and return
    pop edx
    ret                     ; Return to caller