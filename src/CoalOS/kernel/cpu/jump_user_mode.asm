; void jump_to_user_mode(uint32_t kernel_esp)
;
; Switch to a kernel stack prepared with an IRET frame (EIP, CS, EFLAGS,
; user ESP, SS) and enter user mode.

section .text
global jump_to_user_mode

jump_to_user_mode:
    mov     esp, [esp + 4]
    iret
