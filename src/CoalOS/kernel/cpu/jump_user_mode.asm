; jump_user_mode.asm - Jump to user mode using IRET
;
; void jump_to_user_mode(uint32_t kernel_esp);
;
; This function switches to the kernel stack pointer prepared by
; prepare_initial_kernel_stack() and executes IRET to transition to user mode.

section .text
global jump_to_user_mode

jump_to_user_mode:
    ; Get parameter: kernel_esp (prepared stack pointer)
    mov eax, [esp + 4]    ; kernel_esp
    
    ; Debug: Output a character to serial port before IRET
    push eax
    mov dx, 0x3f8         ; COM1 port
    mov al, 'J'           ; Character to indicate we reached jump
    out dx, al
    pop eax
    
    ; Switch to the prepared kernel stack
    ; This stack contains (from top to bottom):
    ; - User EIP
    ; - User CS  
    ; - EFLAGS
    ; - User ESP
    ; - User SS
    mov esp, eax
    
    ; Execute IRET to jump to user mode
    ; IRET pops EIP, CS, EFLAGS, ESP, SS from stack and switches to user mode
    iret
    
    ; This point should never be reached
    mov dx, 0x3f8         ; COM1 port
    mov al, 'X'           ; Should never see this
    out dx, al
    hlt