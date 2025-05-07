; -----------------------------------------------------------------------------
; syscall.asm -- INT 0x80 Entry/Exit Stub for UiAOS
; Version: 6.0
; Author: Tor Martin Kohle
; Purpose:
;   Provides the low-level assembly interface for system calls initiated via
;   the INT 0x80 software interrupt. It constructs the C-callable stack frame
;   (isr_frame_t), switches to kernel segments, dispatches to the C handler
;   (syscall_dispatcher), and correctly restores user-space context, ensuring
;   the syscall return value (in EAX) is preserved for the user process.
;
; Stack Frame upon entry to C handler (syscall_dispatcher):
;   [ESP      ] -> &isr_frame_t (argument for C handler)
;   [ESP + 4  ] -> Return address for call to C handler
;   [ESP + 8  ] -> (Actual start of isr_frame_t) EAX_saved_by_pusha
;   [ESP + 12 ] -> ECX_saved_by_pusha
;   [ESP + 16 ] -> EDX_saved_by_pusha
;   [ESP + 20 ] -> EBX_saved_by_pusha
;   [ESP + 24 ] -> ESP_original_saved_by_pusha (dummy)
;   [ESP + 28 ] -> EBP_saved_by_pusha
;   [ESP + 32 ] -> ESI_saved_by_pusha
;   [ESP + 36 ] -> EDI_saved_by_pusha
;   [ESP + 40 ] -> GS_original
;   [ESP + 44 ] -> FS_original
;   [ESP + 48 ] -> ES_original
;   [ESP + 52 ] -> DS_original
;   [ESP + 56 ] -> int_no (0x80)
;   [ESP + 60 ] -> err_code (dummy 0)
;   [ESP + 64 ] -> EIP_user
;   [ESP + 68 ] -> CS_user
;   [ESP + 72 ] -> EFLAGS_user
;   [ESP + 76 ] -> ESP_user (if CPL change)
;   [ESP + 80 ] -> SS_user  (if CPL change)
;
;   Note: Offsets for isr_frame_t members relative to its base (pointed by ESP in C handler):
;    isr_frame.eax       is at [ESP + 0] in C handler, or [ESP + 8] in this asm before C call.
;    isr_frame.int_no    is at [ESP + 40] in C handler.
;    isr_frame.err_code  is at [ESP + 44] in C handler.
;    isr_frame.eip       is at [ESP + 48] in C handler.
; -----------------------------------------------------------------------------

%define KERNEL_DATA_SELECTOR 0x10
; USER_DATA_SELECTOR is not directly used here, but defined for completeness if needed.
; %define USER_DATA_SELECTOR   0x23

    extern syscall_dispatcher     ; C-level syscall handler
    ; extern serial_putc_asm        ; Optional: for ultra-low-level debug
    ; extern serial_print_hex_asm   ; Optional: for ultra-low-level debug

    section .text
    global syscall_handler_asm

syscall_handler_asm:
    ; --- 1. Construct part of the isr_frame_t: Push error code (dummy) and int_no ---
    ; CPU does NOT push an error code for INT 0x80.
    push dword 0            ; Dummy Error Code (err_code for isr_frame_t)
    push dword 0x80         ; Interrupt Number (int_no for isr_frame_t)

    ; --- 2. Save User Segment Registers (DS, ES, FS, GS) ---
    ; These are part of the isr_frame_t and must be saved before PUSHA
    ; to match the expected layout.
    push ds
    push es
    push fs
    push gs

    ; --- 3. Save General-Purpose Registers (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) ---
    pusha                   ; Pushes EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX
                            ; EAX here is the syscall number from the user.
                            ; ESP_orig is the ESP value *before* PUSHA.

    ; --- (Optional) Minimal Entry Trace ---
    ; mov al, '<'
    ; call serial_putc_asm

    ; --- 4. Switch to Kernel Data Segments ---
    ; Crucial for ensuring kernel code operates with correct CPL0 segment permissions.
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax              ; GS is often used for per-CPU data or thread-local storage.

    ; --- 5. Call C-level Dispatcher ---
    ; The current ESP points to the saved EDI (start of PUSHA block), which forms
    ; the base of the `isr_frame_t` structure. The `isr_frame_t` definition
    ; must match this stack layout precisely.
    mov eax, esp            ; EAX = pointer to the on-stack isr_frame_t
    push eax                ; Pass &isr_frame_t as the argument to syscall_dispatcher
    call syscall_dispatcher   ; Returns result in EAX (e.g., FD for open, bytes for read/write)
    add  esp, 4             ; Clean up argument from stack (pops the pushed EAX)

    ; At this point, EAX holds the syscall's return value from the C dispatcher.
    ; This value needs to be placed into the EAX slot of the saved PUSHA frame
    ; so that POPA will restore it correctly for the user process.

    ; --- (Optional) Trace Syscall Return Value (Requires careful register preservation) ---
    ; This block is for debugging the return value *before* it's written to the stack frame.
    ; pushad                  ; Save all registers (including current EAX which has the syscall return)
    ; mov ebx, [esp + 28]     ; Get the syscall return value (now at EAX slot of pushad) into EBX
    ; mov al, 'R'
    ; call serial_putc_asm
    ; mov eax, ebx            ; Load value to print into EAX
    ; call serial_print_hex_asm
    ; popad                   ; Restore all registers (EAX now has the syscall return again)

    ; --- 6. Store Syscall Return Value into the Stack Frame ---
    ; The PUSHA frame is still on the stack. ESP points to the saved EDI.
    ; The EAX slot within the PUSHA frame is at ESP + 28 bytes:
    ;   EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX (7 * 4 = 28 bytes)
    ;   Then comes the EAX slot.
    mov [esp + 28], eax     ; Write the syscall's return value (from C dispatcher, currently in EAX)
                            ; into the EAX slot of the PUSHA frame on the stack.

    ; --- 7. Restore Registers and Segments ---
    ; These are popped in the reverse order they were pushed.
    popa                    ; Restores EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX,
                            ; and importantly, EAX (which now contains the syscall result
                            ; that we just wrote into the frame).

    ; --- (Optional) Trace EAX *after* POPA to verify ---
    ; pushad
    ; mov ebx, eax            ; EAX after popa
    ; mov al, 'L'
    ; call serial_putc_asm
    ; mov eax, ebx
    ; call serial_print_hex_asm
    ; popad

    pop gs                  ; Restore original user GS
    pop fs                  ; Restore original user FS
    pop es                  ; Restore original user ES
    pop ds                  ; Restore original user DS

    add esp, 8              ; Pop int_no and err_code (dummy) from the stack.

    ; --- (Optional) Minimal Exit Trace ---
    ; push eax                ; Save syscall result
    ; mov al, '>'
    ; call serial_putc_asm
    ; pop eax                 ; Restore syscall result

    ; --- 8. Return to User Mode ---
    iret                    ; Pops EIP_user, CS_user, EFLAGS_user, (ESP_user, SS_user if CPL change).
                            ; EAX correctly holds the syscall return value for the user process.