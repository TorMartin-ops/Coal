; src/isr_pf.asm - Page Fault ISR Stub (#14)
; MODIFIED for Exception Handling via exception table and Panic via C helper call.

section .text
bits 32                 ; Ensure assembly for 32-bit mode

; External C function references
global isr14                   ; Export symbol for IDT registration
extern page_fault_handler     ; C handler for user faults / potentially complex kernel faults
extern find_exception_fixup   ; C function to search the exception table
extern invoke_kernel_panic_from_isr ; C helper function that calls KERNEL_PANIC_HALT
extern serial_putc_asm ; External ASM function for serial output

; Define the Kernel Code Segment selector value from your GDT
%define KERNEL_CODE_SELECTOR 0x08 ; Common value, adjust if yours is different

isr14:
    mov al, 'F' ; 'F' for Page Fault
    call serial_putc_asm
    ; CPU pushes ErrorCode, EIP, CS, EFLAGS, [SS_user], [ESP_user] automatically
    ; --- DEBUG: Print 'P' for Page Fault Entry ---
    pusha               ; Save regs temporarily
    mov al, 'P'
    call serial_putc_asm
    popa                ; Restore regs
    ; --- End DEBUG ---

    ; CPU pushes (bottom->top): [SS_user], [ESP_user], EFLAGS, CS, EIP, ErrorCode
    ; We push manually (bottom->top): Interrupt Number (14)

    push dword 14       ; Push interrupt number (vector)

    ; 1. Save general purpose registers
    pusha               ; Pushes EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX (32 bytes)

    ; 2. Save segment registers
    push ds
    push es
    push fs
    push gs



    ; --- Stack Layout Confirmation ... (rest of the file remains the same) ---

    ; --- Check if fault occurred in Kernel (CPL=0) or User mode (CPL=3) ---
    mov ax, word [esp + 52] ; Get CS from stack (offset adjusted for our push 14)
    cmp ax, KERNEL_CODE_SELECTOR
    jne user_fault          ; If CS != Kernel CS, handle as user fault

; --- Kernel Mode Fault ---
    mov edi, [esp + 48]     ; Get faulting EIP (offset adjusted for our push 14)
    push edi
    call find_exception_fixup
    add esp, 4
    test eax, eax
    jnz handle_fixup

kernel_fault_unhandled:
    jmp kernel_panic_pf

handle_fixup:
    mov ebx, [esp + 40]     ; Get saved ECX (remaining count) into EBX
    mov [esp + 44], ebx     ; Set saved EAX = remaining count (now in EBX)
    mov [esp + 56], eax     ; Set saved EIP = fixup_addr (from find_exception_fixup return in EAX)
    jmp restore_and_return

user_fault:
    mov eax, esp            ; Pass stack frame pointer
    push eax
    call page_fault_handler
    add esp, 4
    jmp restore_and_return

kernel_panic_pf:
    call invoke_kernel_panic_from_isr
    cli
    hlt

restore_and_return:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8              ; Pop IntNum + ErrorCode
    iret