; context_switch.asm
; Implements:
;   void context_switch(uint32_t **old_esp, uint32_t *new_esp);
; This routine saves the current CPU context and restores the context for the new task.
; It is written as a naked function (no automatic prologue/epilogue).

global context_switch
section .text
context_switch:
    ; Save general-purpose registers and EFLAGS
    pushad
    pushfd

    ; At this point, the stack frame looks like:
    ; [esp]         -> EFLAGS (4 bytes)
    ; [esp+4]       -> EDI
    ; [esp+8]       -> ESI
    ; [esp+12]      -> EBP
    ; [esp+16]      -> (Old ESP, not used)
    ; [esp+20]      -> EBX
    ; [esp+24]      -> EDX
    ; [esp+28]      -> ECX
    ; [esp+32]      -> EAX
    ; The caller’s stack (arguments) is pushed further down:
    ;   [esp+36] -> Pointer to old_esp (uint32_t **)
    ;   [esp+40] -> new_esp (uint32_t *)
    
    mov eax, [esp + 36]   ; Load pointer to old_esp
    mov edx, [esp + 40]   ; Load new_esp
    mov [eax], esp        ; Save current stack pointer (context frame) into *old_esp

    ; Switch to the new task's context by updating ESP.
    mov esp, edx

    ; Restore the new task's CPU context (registers and flags)
    popfd
    popad

    ret
