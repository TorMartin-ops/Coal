; src/isr_pf.asm - Page Fault ISR Stub (#14)
; MODIFIED for Exception Handling via exception table and Panic via C helper call.

section .text
bits 32                 ; Ensure assembly for 32-bit mode

; External C function references
global isr14                   ; Export symbol for IDT registration
extern page_fault_handler     ; C handler for user faults / potentially complex kernel faults
extern find_exception_fixup   ; C function to search the exception table
extern invoke_kernel_panic_from_isr ; C helper function that calls KERNEL_PANIC_HALT

; Define the Kernel Code Segment selector value from your GDT
%define KERNEL_CODE_SELECTOR 0x08 ; Common value, adjust if yours is different

isr14:
    ; CPU pushes (bottom->top): [SS_user], [ESP_user], EFLAGS, CS, EIP, ErrorCode
    ; We push manually (bottom->top): Interrupt Number (14)

    push dword 14       ; Push interrupt number (vector)

    ; 1. Save general purpose registers
    pusha               ; Pushes EDI, ESI, EBP, ESP_orig, EBX, EDX, ECX, EAX (32 bytes)

    ; 2. Save segment registers (use push word for 16-bit selectors)
    push ds             ; 2 bytes pushed, address increments by 4 due to stack alignment/mode? Check docs. Usually ESP decreases by 2. Let's assume ESP decreases by 2 for each.
    push es             ; 2 bytes
    push fs             ; 2 bytes
    push gs             ; 2 bytes (Total 8 bytes pushed for segments)

    ; --- Stack Layout Confirmation (Relative to current ESP after all pushes): ---
    ; ESP+0 : GS (word)
    ; ESP+2 : FS (word)
    ; ESP+4 : ES (word)
    ; ESP+6 : DS (word)
    ; ESP+8 : EAX (dword, from pusha)
    ; ESP+12: ECX (dword)
    ; ESP+16: EDX (dword)
    ; ESP+20: EBX (dword)
    ; ESP+24: ESP_original (dword)
    ; ESP+28: EBP (dword)
    ; ESP+32: ESI (dword)
    ; ESP+36: EDI (dword)
    ; --- Base of PUSHA frame relative to ESP = ESP+8 ---
    ; --- Base of Segments relative to ESP = ESP+0 ---
    ; ESP+40: Interrupt Number (dword, pushed by us)
    ; ESP+44: Error Code (dword, pushed by CPU)
    ; ESP+48: EIP (dword, pushed by CPU - Faulting instruction pointer)
    ; ESP+52: CS (dword, pushed by CPU - lower 16 bits relevant)
    ; ESP+56: EFLAGS (dword, pushed by CPU)
    ; ESP+60: ESP_user (dword, only if CPL changed from user to kernel)
    ; ESP+64: SS_user (dword, only if CPL changed from user to kernel)
    ; --------------------------------------------------------------
    ; *** OFFSETS ARE CRITICAL - VERIFY CAREFULLY ***

    ; --- Check if fault occurred in Kernel (CPL=0) or User mode (CPL=3) ---
    ; Check the CS selector pushed by the CPU
    mov ax, word [esp + 52] ; Get CS from stack (lower 16 bits)
    cmp ax, KERNEL_CODE_SELECTOR
    jne user_fault          ; If CS != Kernel CS, handle as user fault (jump to label below)

; --- Kernel Mode Fault ---
    ; This is unexpected *unless* it's from our uaccess code trying to access user memory.
    ; Check the exception table for the faulting instruction pointer (EIP).

    mov edi, [esp + 48]     ; Get faulting EIP (offset verified above)

    ; Call C function to check exception table
    ; Assumes standard C calling convention (caller cleans stack)
    push edi                ; Push EIP as argument for find_exception_fixup
    call find_exception_fixup
    add esp, 4              ; Clean up argument from stack
    ; EAX now holds the fixup address (non-zero) or 0 if not found

    test eax, eax           ; Check if EAX is zero (fixup address found?)
    jnz handle_fixup        ; Found an entry (jump to handle_fixup label), EAX has fixup addr

kernel_fault_unhandled:
    ; --- No Fixup Found: Genuine Kernel Fault ---
    ; This indicates a bug in the kernel itself. Panic immediately.
    ; Jump to the panic routine label.
    jmp kernel_panic_pf     ; Go to panic routine (defined below)

handle_fixup:
    ; --- Recoverable Fault in uaccess ---
    ; EAX contains the fixup_addr returned by find_exception_fixup.
    ; Set the return value (bytes not copied = remaining count from ECX) in the *saved* EAX slot.
    ; Set the *saved* EIP to the fixup address so IRET returns there.

    ; Get remaining count from saved ECX (offset from pusha frame)
    mov ebx, [esp + 12]     ; Get saved ECX (offset verified above)

    ; Set return value in saved EAX slot on the stack
    mov [esp + 8], ebx      ; Set saved EAX = remaining count (offset verified above)

    ; Adjust saved EIP on the stack to the fixup address stored in EAX
    mov [esp + 48], eax     ; Set saved EIP = fixup_addr (offset verified above)

    ; Now restore registers and iret to the fixup address in uaccess.asm
    jmp restore_and_return  ; Jump to common restore path (defined below)

user_fault:
    ; --- Fault Occurred in User Mode ---
    ; This is expected for many reasons (demand paging, copy-on-write, protection error).
    ; Pass the context (ESP points to the saved state) to the C handler for analysis.
    mov eax, esp            ; Pass stack frame pointer as argument to C handler
    push eax
    call page_fault_handler ; C handler should analyze error code, CR2, VMA etc.
                            ; It might map pages, handle CoW, or decide to terminate the process.
    add esp, 4              ; Clean up C argument from stack

    ; If page_fault_handler fixed the issue (e.g., mapped a page), it returns normally.
    ; If it decided to terminate the process, it should arrange for the scheduler
    ; to handle that, but still return here so we can IRET cleanly (perhaps to a
    ; process termination stub). For now, assume it returns if fixable.
    jmp restore_and_return  ; Jump to common restore path

kernel_panic_pf:
    ; This label handles the case of an unhandled kernel page fault.
    ; It calls the C wrapper which invokes KERNEL_PANIC_HALT.
    call invoke_kernel_panic_from_isr ; Call the C helper function

    ; The C helper function invoke_kernel_panic_from_isr should NOT return,
    ; as it halts the system. But add cli/hlt just in case something goes wrong.
    cli                     ; Disable interrupts
    hlt                     ; Halt CPU (Should be unreachable)

restore_and_return:         ; Label for common restore path
    ; 4. Restore segment registers (in reverse order of push)
    pop gs
    pop fs
    pop es
    pop ds

    ; 5. Restore general registers (popa restores in reverse order of pusha)
    popa

    ; 6. Discard interrupt number (pushed by us) AND error code (pushed by CPU)
    add esp, 8              ; Pop 2 * dword (8 bytes)

    ; 7. Return from interrupt
    ; Pops EIP, CS, EFLAGS [, ESP_user, SS_user if CPL changed] from stack
    ; Jumps to the popped EIP (either original user EIP, original kernel EIP,
    ; or the adjusted fixup EIP for uaccess faults).
    iret