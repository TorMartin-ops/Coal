; setup_idle_context.asm - Create proper context for idle task
;
; uint32_t* setup_idle_context(uint32_t* stack_top, void (*idle_func)(void));
;
; This function sets up a stack frame that exactly matches what simple_switch
; would create, so the idle task can be properly restored.

section .text
global setup_idle_context

setup_idle_context:
    ; Save current registers we'll modify
    push ebp
    mov ebp, esp
    push eax
    push ebx
    push edx
    
    ; Get parameters
    mov eax, [ebp + 8]     ; stack_top
    mov ebx, [ebp + 12]    ; idle_func
    
    ; Set up the idle task's stack to look like simple_switch saved it
    ; We'll build the stack frame manually in reverse order
    
    ; Start from stack_top and work downward
    mov edx, eax           ; edx = current stack pointer
    
    ; Push return address (where simple_switch's ret will go)
    sub edx, 4
    mov [edx], ebx         ; idle_func address
    
    ; Push EFLAGS (what simple_switch's pushfd saved)
    sub edx, 4
    mov dword [edx], 0x202 ; IF=1, reserved bit 1=1
    
    ; Push general purpose registers (what simple_switch's pushad saved)
    ; pushad saves: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    ; so we need to push them in reverse order for popad to restore correctly
    sub edx, 4
    mov dword [edx], 0     ; EAX
    sub edx, 4  
    mov dword [edx], 0     ; ECX
    sub edx, 4
    mov dword [edx], 0     ; EDX  
    sub edx, 4
    mov dword [edx], 0     ; EBX
    sub edx, 4
    mov dword [edx], 0     ; ESP (ignored by popad)
    sub edx, 4
    mov dword [edx], 0     ; EBP
    sub edx, 4
    mov dword [edx], 0     ; ESI
    sub edx, 4
    mov dword [edx], 0     ; EDI
    
    ; Push segment registers (what simple_switch saved)
    ; These are pushed: ds, es, fs, gs (so will be popped: gs, fs, es, ds)
    sub edx, 4
    mov dword [edx], 0x10  ; DS (KERNEL_DATA_SEL)
    sub edx, 4  
    mov dword [edx], 0x10  ; ES
    sub edx, 4
    mov dword [edx], 0x10  ; FS
    sub edx, 4
    mov dword [edx], 0x10  ; GS
    
    ; EDX now points to the complete saved context
    ; Return this as the context pointer
    mov eax, edx
    
    ; Restore our saved registers
    pop edx
    pop ebx
    pop eax
    pop ebp
    ret