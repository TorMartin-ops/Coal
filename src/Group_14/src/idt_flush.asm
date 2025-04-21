; idt_flush.asm
; Loads the IDT register (IDTR).
; This function is typically called after the IDT has been initialized in C.

section .text
bits 32

global idt_flush ; Export the symbol for the linker

; Function signature expected from C:
; void idt_flush(uint32_t idt_ptr_addr);
;
; idt_ptr_addr: This is the virtual address of the 'idt_ptr' structure
;               which contains the limit (size) and base address of the IDT.
;               The C code passes this address on the stack.

idt_flush:
    mov eax, [esp + 4]  ; Get the argument (idt_ptr_addr) from the stack (ESP+4 for cdecl)
    lidt [eax]          ; Load the IDT register (IDTR) using the pointer
                        ; The lidt instruction expects the memory operand to be the
                        ; address of the 6-byte idt_ptr structure (16-bit limit, 32-bit base).
    ret                 ; Return to the C caller (idt_init)