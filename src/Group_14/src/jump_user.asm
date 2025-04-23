; src/jump_user.asm
; Jumps to user mode using IRET after setting up the stack.
section .text
bits 32

global jump_to_user_mode
extern serial_putc_asm ; For debug output

; Function signature:
; void jump_to_user_mode(uint32_t *kernel_stack_ptr, uint32_t *page_directory_phys);
; Args:
;   [ebp+8]  = kernel_stack_ptr (points to prepared iret frame)
;   [ebp+12] = page_directory_phys (physical address of user PD)

jump_to_user_mode:
    cli             ; Disable interrupts before critical state change

    push ebp
    mov ebp, esp

    ; --- DEBUG: Entry ---
    pusha
    mov al, 'J'
    call serial_putc_asm
    mov al, '1'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    ; Get arguments
    mov edx, [ebp + 8]  ; kernel_stack_ptr (points to prepared iret frame)
    mov eax, [ebp + 12] ; page_directory_phys

    ; --- DEBUG: Args Loaded ---
    pusha
    mov al, 'J'
    call serial_putc_asm
    mov al, '2'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    ; Switch Page Directory if needed
    mov ecx, cr3        ; Get current CR3
    cmp eax, ecx        ; Compare with new physical address
    je .skip_cr3_load   ; Skip if same
    test eax, eax       ; Skip if new address is NULL (shouldn't be)
    jz .skip_cr3_load

    ; Safety mask CR3 address to ensure lower bits are 0 (as required by CPU)
    and eax, 0xFFFFF000

    ; --- DEBUG: Before CR3 Load ---
    pusha
    mov al, 'C'
    call serial_putc_asm
    mov al, '3'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    mov cr3, eax        ; Load process page directory physical address

    ; --- DEBUG: After CR3 Load ---
    pusha
    mov al, 'C'
    call serial_putc_asm
    mov al, '4'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

.skip_cr3_load:
    ; --- DEBUG: CR3 Load Skipped/Done ---
    pusha
    mov al, 'C'
    call serial_putc_asm
    mov al, '5'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    ; Switch stack pointer to the prepared frame location saved in edx
    mov esp, edx

    ; --- DEBUG: ESP Switched ---
    pusha
    mov al, 'E'
    call serial_putc_asm
    mov al, 'S'
    call serial_putc_asm
    mov al, 'P'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    ; Execute IRET. This performs the privilege level change and jumps to user mode.
    ; It pops EIP, CS, EFLAGS, ESP, SS from the stack located by the new ESP.
    ; hlt             ; <<< --- REMOVED THIS HLT --- >>>

    ; --- DEBUG: Before IRET ---
    pusha
    mov al, 'I'
    call serial_putc_asm
    mov al, 'R'
    call serial_putc_asm
    mov al, 'E'
    call serial_putc_asm
    mov al, 'T'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---

    iret

    ; --- DEBUG: After IRET (Should NOT be reached) ---
    pusha
    mov al, '!' ; Indicate failure
    call serial_putc_asm
    mov al, 'F'
    call serial_putc_asm
    mov al, 'A'
    call serial_putc_asm
    mov al, 'I'
    call serial_putc_asm
    mov al, 'L'
    call serial_putc_asm
    popa
    ; --- End DEBUG ---


    ; Execution should not reach here if iret successfully transitions to user mode.
.fail:
    ; If iret fails catastrophically
    ; or somehow returns (which it shouldn't), halt the system.
    cli
.halt_loop:
    hlt
    jmp .halt_loop

