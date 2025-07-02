; -----------------------------------------------------------------------------
; syscall.asm -- INT 0x80 Entry/Exit Stub for CoalOS (v6.1 - Reschedule Check)
; Version: 6.1
; Author: Tor Martin Kohle
; Purpose:
;   Provides the low-level assembly interface for system calls initiated via
;   the INT 0x80 software interrupt. It constructs the C-callable stack frame
;   (isr_frame_t), switches to kernel segments, dispatches to the C handler
;   (syscall_dispatcher), checks if a reschedule is needed, potentially calls
;   the scheduler, and correctly restores user-space context, ensuring
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
    extern schedule             ; <<< ADDED: External C scheduler function
    extern g_need_reschedule    ; <<< ADDED: External C reschedule flag variable (volatile bool -> byte)
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
    push ds
    push es
    push fs
    push gs

    ; --- 3. Save General-Purpose Registers (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) ---
    pusha                   ; Pushes EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX
                            ; EAX here is the syscall number from the user.

    ; --- 4. Switch to Kernel Data Segments ---
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- 5. Call C-level Dispatcher ---
    mov eax, esp            ; EAX = pointer to the on-stack isr_frame_t
    push eax                ; Pass &isr_frame_t as the argument to syscall_dispatcher
    call syscall_dispatcher   ; Returns result in EAX
    add  esp, 4             ; Clean up argument stack

    ; EAX now holds the syscall's return value

    ; --- 6. Store Syscall Return Value into the Stack Frame ---
    mov [esp + 28], eax     ; Write return value into the EAX slot of the PUSHA frame

    ; --- *** 7. CHECK RESCHEDULE FLAG (Interrupts are still OFF from int 0x80) *** ---
check_reschedule:
    ; Access the global C variable (byte for bool)
    mov al, byte [g_need_reschedule]
    test al, al                      ; Check if the flag is non-zero
    jz .no_reschedule_needed         ; If zero, skip the schedule call

    ; Reschedule is needed:
    mov byte [g_need_reschedule], 0  ; Clear the flag (no lock needed, IF=0)
    call schedule                    ; Call the C scheduler function. It handles context switch.

.no_reschedule_needed:
    ; --- 8. Restore Registers and Segments ---
    popa                    ; Restores EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX (with syscall result)
    pop gs
    pop fs
    pop es
    pop ds

    ; --- 9. Clean up vector number and error code ---
    add esp, 8              ; Pop int_no and dummy error code

    ; --- 10. Return to User Mode ---
    iret                    ; Pops EIP_user, CS_user, EFLAGS_user, [ESP_user], [SS_user]
                            ; Returns control to the user process with EAX holding the result.
