; src/jump_user.asm
; Jumps to user mode using IRET after setting up the stack.

section .text
bits 32

global jump_to_user_mode

; Function signature:
; void jump_to_user_mode(uint32_t *kernel_stack_ptr, uint32_t *page_directory_phys);
; Args:
;   [ebp+8]  = kernel_stack_ptr (points to prepared iret frame)
;   [ebp+12] = page_directory_phys (physical address of user PD)

jump_to_user_mode:
    cli             ; Disable interrupts before critical state change

    push ebp
    mov ebp, esp

    ; Get arguments
    mov edx, [ebp + 8]  ; kernel_stack_ptr (points to prepared iret frame)
    mov eax, [ebp + 12] ; page_directory_phys

    ; Switch Page Directory if needed
    mov ecx, cr3        ; Get current CR3
    cmp eax, ecx        ; Compare with new physical address
    je .skip_cr3_load   ; Skip if same
    test eax, eax       ; Skip if new address is NULL (shouldn't be)
    jz .skip_cr3_load
    and eax, 0xFFFFF000 ; Safety mask CR3 address
    mov cr3, eax        ; Load process page directory physical address

.skip_cr3_load:
    ; Switch stack pointer to the prepared frame location saved in edx
    mov esp, edx        ; ESP now points to the IRET frame

    ; Execute IRET. Pops EIP, CS, EFLAGS, ESP, SS from stack at current ESP.
    iret

    ; Execution should not reach here if iret successfully transitions to user mode.
.fail:
    ; If iret fails catastrophically or somehow returns to kernel mode, halt the system.
    cli
.halt_loop:
    hlt
    jmp .halt_loop