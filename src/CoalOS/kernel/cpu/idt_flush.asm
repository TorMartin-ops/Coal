; void idt_flush(uintptr_t idt_ptr_addr): load the IDT register.

section .text
global idt_flush

idt_flush:
    mov     eax, [esp + 4]
    lidt    [eax]
    ret
