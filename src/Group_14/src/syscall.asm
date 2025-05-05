; syscall.asm - v4.6 (Corrected Segment Handling on Return)
; Implements the fix identified by the user: discard saved user segments
; instead of popping them at CPL 0. Matches the isr_frame_t structure.

section .text
global syscall_handler_asm
extern syscall_dispatcher
; extern serial_putc_asm ; Uncomment if debug prints needed

%define KERNEL_DATA_SELECTOR 0x10

syscall_handler_asm:
    ; 1. Push dummy error code (0)
    push dword 0

    ; 2. Push interrupt number (0x80 for syscall)
    push dword 0x80

    ; 3. Save segment registers (DS, ES, FS, GS) - User values
    push ds
    push es
    push fs
    push gs

    ; 4. Save all general purpose registers using pusha
    pusha           ; Saves EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX
                    ; EDI is at [esp+0], EAX is at [esp+28] relative to current ESP.

    ; --- Set up Kernel Data Segments ---
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- Call C syscall dispatcher ---
    ; Pass pointer to the base of the isr_frame_t struct (saved EDI).
    mov eax, esp        ; EAX = Current ESP (points to saved EDI)
    push eax            ; Push pointer to frame (regs*) as the single argument
    call syscall_dispatcher ; Calls void syscall_dispatcher(isr_frame_t *regs)
    add esp, 4          ; Clean up argument

    ; C dispatcher places return value in EAX AND modifies regs->eax on the stack

    ; --- Store return value (already in EAX from C call) into the frame's EAX slot ---
    ; The EAX slot relative to ESP *after* pusha is at offset +28.
    mov [esp + 28], eax ; Store return value in the correct saved EAX slot for popa

    ; --- Restore GP Registers ---
    popa                ; Restore general purpose registers (EAX gets return value from stack)

    ; --- Drop saved user GS/FS/ES/DS without loading them ---
    ; This is the CRUCIAL FIX: Popping user segments (RPL=3) at CPL=0 is illegal.
    add esp, 16         ; Skip 4xDWORD for gs, fs, es, ds

    ; --- Pop IntNum + ErrorCode ---
    add esp, 8          ; Pop IntNum + ErrorCode

    ; --- Return to User Space ---
    iret                ; Hardware loads user segments (CS, SS) and GP registers safely
                        ; as part of the privilege level transition. Implicit DS/ES/FS/GS
                        ; are typically loaded based on the new SS descriptor.