; start_idle.asm - Simple function to start the idle task
;
; void start_idle_task(void (*idle_func)(void));
;
; This function simply jumps to the idle task function without
; complex context switching logic.

section .text
global start_idle_task

start_idle_task:
    ; Get parameter: idle function pointer
    mov eax, [esp + 4]    ; idle_func
    
    ; Set up proper segment registers
    mov bx, 0x10          ; KERNEL_DATA_SEL
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    
    ; Set up a clean stack frame
    ; Clear all registers for consistency
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    
    ; Enable interrupts and call the idle function
    sti
    call eax
    
    ; Should never reach here since idle task is infinite loop
    hlt