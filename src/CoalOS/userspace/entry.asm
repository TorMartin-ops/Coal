; entry.asm - User-space program entry point for CoalOS
; Author: Tor Martin Kohle
;
; Purpose: This file defines the _start symbol, which is the conventional entry
; point for user-space applications. It calls the C 'main' function and then
; uses its return value to perform a system call for exiting the process.

section .text
bits 32         ; Target 32-bit protected mode.

global _start   ; Export _start symbol, making it the linker's entry point.
extern main     ; Import the C 'main' function, which contains the program logic.

; System call number for exiting the process. Must match kernel's definition.
SYS_EXIT equ 1

_start:
    ; The kernel is responsible for setting up the initial stack environment,
    ; including any arguments (argc, argv) and environment variables, before
    ; transferring control to this _start routine. This simple entry point
    ; does not explicitly process argc/argv.
    
    ; First instruction: just a NOP to see if we even execute one instruction
    nop
    
    ; Debug: Make a simple syscall first to verify we reached user space
    mov eax, 7      ; SYS_PUTS
    mov ebx, debug_msg
    int 0x80

    call main       ; Invoke the C main function.
                    ; Per System V i386 ABI, 'main' returns its result in EAX.

    ; --- Process Exit Sequence ---
    ; The value returned by 'main' (in EAX) is used as the exit status code.
    mov ebx, eax    ; First syscall argument (exit code) goes into EBX.
    mov eax, SYS_EXIT ; Load the syscall number for exit into EAX.

    ; Optional: Zero out other registers used for syscall arguments (ECX, EDX)
    ; for this specific syscall, as they are not used by SYS_EXIT.
    xor ecx, ecx    ; Second syscall argument (unused).
    xor edx, edx    ; Third syscall argument (unused).

    int 0x80        ; Trigger software interrupt 0x80 to request kernel service.
                    ; A functional SYS_EXIT syscall should not return control here.

; Fallback hang loop:
; If the SYS_EXIT syscall fails or if execution unexpectedly reaches this
; point, this loop will halt the CPU to prevent undefined behavior.
hang:
    cli             ; Disable interrupts as a safety measure.
    hlt             ; Halt the processor.
    jmp hang        ; Loop indefinitely if somehow resumed.

section .data
debug_msg db "DEBUG: Reached _start in userspace!", 0