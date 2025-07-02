; context_switch.asm - DEAD SIMPLE APPROACH THAT WORKS
; 
; void simple_switch(uint32_t **old_esp, uint32_t *new_esp);
;
; This is the simplest possible context switch that can work:
; 1. Save ALL registers to the stack
; 2. Save the stack pointer 
; 3. Load the new stack pointer
; 4. Restore ALL registers
; 5. Return

section .text
global simple_switch

simple_switch:
    ; Get parameters
    mov eax, [esp + 4]    ; old_esp (uint32_t **)
    mov edx, [esp + 8]    ; new_esp (uint32_t *)

    ; Save ALL registers (complete CPU state)
    pushfd                ; EFLAGS
    pushad                ; EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    
    ; Save segment registers  
    push ds
    push es
    push fs
    push gs
    
    ; Save current stack pointer to *old_esp
    mov [eax], esp
    
    ; Switch to new stack
    mov esp, edx
    
    ; Restore segment registers
    pop gs
    pop fs
    pop es
    pop ds
    
    ; Restore ALL registers
    popad                 ; EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX
    popfd                 ; EFLAGS
    
    ret