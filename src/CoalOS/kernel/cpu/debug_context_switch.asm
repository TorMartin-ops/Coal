; debug_context_switch.asm - Instrumented version of simple_switch for debugging
;
; void debug_simple_switch(uint32_t **old_esp, uint32_t *new_esp);
;
; This is an instrumented version that prints debug info at each step

section .text
global debug_simple_switch
extern serial_write

debug_simple_switch:
    ; Print entry message
    push eax
    push msg_entry
    call serial_write
    add esp, 4
    pop eax
    
    ; Get parameters
    mov eax, [esp + 4]    ; old_esp (uint32_t **)
    mov edx, [esp + 8]    ; new_esp (uint32_t *)

    ; Print new_esp value
    push eax
    push edx
    push ecx
    push msg_new_esp
    call serial_write
    add esp, 4
    ; Print hex value of new_esp (simplified)
    pop ecx
    pop edx
    pop eax

    ; Save ALL registers (complete CPU state)
    push eax
    push msg_saving
    call serial_write
    add esp, 4
    pop eax
    
    pushfd                ; EFLAGS
    pushad                ; EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    
    ; Save segment registers  
    push ds
    push es
    push fs
    push gs
    
    ; Save current stack pointer to *old_esp
    mov [eax], esp
    
    ; Print before stack switch
    push eax
    push msg_switching
    call serial_write
    add esp, 4
    pop eax
    
    ; Switch to new stack
    mov esp, edx
    
    ; Print after stack switch
    push eax
    push msg_restoring
    call serial_write
    add esp, 4
    pop eax
    
    ; Restore segment registers
    pop gs
    pop fs
    pop es
    pop ds
    
    ; Print before popad
    push eax
    push msg_popad
    call serial_write
    add esp, 4
    pop eax
    
    ; Restore ALL registers
    popad                 ; EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX
    
    ; Print before popfd
    push eax
    push msg_popfd
    call serial_write
    add esp, 4
    pop eax
    
    popfd                 ; EFLAGS
    
    ; Print before return
    push eax
    push msg_returning
    call serial_write
    add esp, 4
    pop eax
    
    ret

section .data
msg_entry:     db "[DEBUG CTX] Entering simple_switch", 0xA, 0
msg_new_esp:   db "[DEBUG CTX] New ESP loaded", 0xA, 0  
msg_saving:    db "[DEBUG CTX] Saving context", 0xA, 0
msg_switching: db "[DEBUG CTX] Switching stacks", 0xA, 0
msg_restoring: db "[DEBUG CTX] Restoring context", 0xA, 0
msg_popad:     db "[DEBUG CTX] About to POPAD", 0xA, 0
msg_popfd:     db "[DEBUG CTX] About to POPFD", 0xA, 0
msg_returning: db "[DEBUG CTX] About to RET", 0xA, 0