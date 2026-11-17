; Kernel context switch and the first-run trampoline.
;
; simple_switch saves flags, all registers and the data segments on the old
; kernel stack, stores that stack pointer, loads the new one and unwinds the
; same frame. The frame layout (low to high) is struct switch_frame in
; process.c: gs fs es ds | edi esi ebp esp ebx edx ecx eax | eflags | return.

section .text
global simple_switch
global ret_from_fork

; void simple_switch(uint32_t **save_esp, uint32_t *new_esp)
simple_switch:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    mov [eax], esp
    mov esp, edx
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    ret

; New tasks "return" here with an isr_frame_t on the stack (same layout the
; syscall entry builds), so the first run is an ordinary return to user mode.
ret_from_fork:
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8              ; int_no, err_code
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
