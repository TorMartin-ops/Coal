; entry.asm - Entry point for user-space programs

section .text
global _start      ; Linker entry point

extern main        ; C main function

_start:
    ; No complex setup needed here for a simple program
    ; Stack is already set up by the kernel before jumping here

    call main      ; Call the C main function
                   ; The return value will be in EAX

    ; After main returns, call sys_exit with the return value from main
    mov ebx, eax   ; Move return value (exit code) into EBX for syscall
    mov eax, 1     ; SYS_EXIT syscall number (Updated from 2 to 1)
    int 0x80       ; Invoke kernel syscall

    ; Should not be reached if exit works
hang:
    jmp hang       ; Loop forever if exit fails