; entry.asm - Entry point for user-space programs

section .text
bits 32         ; Ensure 32-bit code generation

global _start   ; Export _start symbol for the linker (ENTRY point)
extern main     ; Import the C main function symbol

; Syscall number definition (consistent with C code)
SYS_EXIT equ 1

_start:
    ; The kernel sets up the stack (arguments, environment) before jumping here.
    ; For this simple program, we don't need to process argc/argv.

    call main       ; Call the C main function.
                    ; By C calling convention (System V i386 ABI),
                    ; the return value is placed in the EAX register.

    ; --- Exit Sequence ---
    ; Use the return value from main as the exit status code.
    mov ebx, eax    ; Move return value (exit code) from EAX into EBX.
                    ; EBX is the first argument register for our syscall convention.
    mov eax, SYS_EXIT ; Load the SYS_EXIT syscall number into EAX.

    ; Optional: Zero out other argument registers for clarity/safety,
    ; although not strictly required by the SYS_EXIT implementation shown.
    xor ecx, ecx    ; Set ECX to 0 (second argument)
    xor edx, edx    ; Set EDX to 0 (third argument)

    int 0x80        ; Invoke kernel syscall trap.
                    ; Control should not return here if SYS_EXIT works.

; Fallback loop: If the exit syscall somehow fails and returns,
; or if execution reaches here unexpectedly, halt the CPU indefinitely.
hang:
    cli             ; Disable interrupts (optional, safety)
    hlt             ; Halt the processor
    jmp hang        ; Loop back to halt if somehow resumed