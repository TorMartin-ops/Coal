section .text
global jump_to_user_mode
; extern after_cr3_test ; Or remove if not using test anymore
; extern serial_putc_asm

; %define USER_DATA_SELECTOR 0x23 ; Definition no longer needed here

jump_to_user_mode:
    cli                 ; Disable interrupts before state change

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

    ; Safety mask CR3 address to ensure lower bits are 0 (as required by CPU)
    and eax, 0xFFFFF000
    mov cr3, eax        ; Load process page directory physical address

.skip_cr3_load:

    ; === MODIFICATION START ===
    ; DO NOT load DS, ES, FS, GS here. Let IRET handle setting up
    ; the segment registers based on the CS and SS popped from the stack.
    ; mov ax, USER_DATA_SELECTOR
    ; mov ds, ax
    ; mov es, ax
    ; mov fs, ax
    ; mov gs, ax
    ; === MODIFICATION END ===

    ; Switch stack pointer to the prepared frame location saved in edx
    mov esp, edx

    ; Execute IRET. This performs the privilege level change and jumps to user mode.
    ; It pops EIP, CS, EFLAGS, ESP, SS from the stack located by the new ESP.
    iret

    ; Execution should not reach here if iret successfully transitions to user mode.
.fail:
    ; If iret fails catastrophically (which it seems to be doing via GPF)
    ; or somehow returns (which it shouldn't), halt the system.
    cli
.halt_loop:
    hlt
    jmp .halt_loop