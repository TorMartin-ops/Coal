; syscall.asm - System Call Handler Assembly Stub - CORRECTED v4 (Minimal)

section .text
global syscall_handler_asm      ; Export symbol for IDT registration
extern syscall_dispatcher      ; C dispatcher function

; Define kernel data segment selector (must match your GDT)
; **** ENSURE THIS VALUE IS CORRECT FOR YOUR GDT ****
%define KERNEL_DATA_SELECTOR 0x10 ; Example value - USE YOUR ACTUAL KERNEL DS

section .text
syscall_handler_asm:
    ; 1. Save original EAX/EBX needed by C dispatcher
    push ebx                    ; Save original EBX (first arg)
    push eax                    ; Save original EAX (syscall num)

    ; 2. Push dummy error code (0)
    push dword 0

    ; 3. Push "interrupt" number (0x80 for syscall)
    push dword 0x80

    ; 4. Save segment registers (DS, ES, FS, GS)
    push ds
    push es
    push fs
    push gs

    ; 5. Save all general purpose registers using pusha
    pusha                       ; Pushes EDI..EAX (32 bytes). EBX value here seems problematic in logs.

    ; --- Set up Kernel Data Segments ---
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- Prepare arguments and Call C syscall dispatcher ---
    ; Stack layout relative to current ESP (immediately after pusha):
    ;   ESP+28 -> Saved EDI from pusha (Start of syscall_regs_t fields)
    ;   ...
    ;   ESP+56 -> Original EAX (Syscall number)
    ;   ESP+60 -> Original EBX (First argument)

    ; Argument 1: Pointer to syscall_regs_t frame.
    lea ecx, [esp + 28]         ; ECX = Pointer to saved EDI (start of struct fields)

    ; Argument 2: Original Syscall Number (EAX)
    mov eax, [esp + 56]         ; EAX = Original EAX (syscall_num)

    ; Argument 3: Original First Argument (EBX)
    mov edx, [esp + 60]         ; EDX = Original EBX (first_arg_ebx)

    ; Push arguments for C call (reverse order)
    push edx                    ; Push original EBX (first_arg_ebx)
    push eax                    ; Push original EAX (syscall_num)
    push ecx                    ; Push pointer to frame (regs)
    call syscall_dispatcher     ; Call void syscall_dispatcher(regs*, num, arg1)
    add esp, 12                 ; Clean up 3 arguments (12 bytes)

    ; C dispatcher places return value in EAX

    ; --- Store return value (in EAX) into the CORRECT stack slot ---
    ; The C function's return value is in EAX. We need to put it where popa expects it.
    mov [esp + 0], eax          ; Store return value in the stack slot for EAX (saved by pusha)

    ; --- Restore Registers and Return ---
    popa                        ; Restore general purpose registers (EAX gets return value)
    pop gs                      ; Restore segments
    pop fs
    pop es
    pop ds
    add esp, 8                  ; Clean up error code & int number
    add esp, 8                  ; Clean up original EAX & EBX (initially pushed)
    iret                        ; Return to user space