; Selectors and the common interrupt frame shared by the assembly stubs.
; The frame layout must match isr_frame_t (include/kernel/cpu/isr_frame.h).

KERNEL_CS   equ 0x08
KERNEL_DS   equ 0x10
USER_CS     equ 0x1B        ; ring 3 selectors, see gdt.h
USER_DS     equ 0x23
VDSO_LANDING equ 0xBFFFF409 ; the sysenter return address; must match vdso_abi.h
IRQ_BASE    equ 32          ; first PIC vector, see include/kernel/cpu/idt.h

; Push segments and general registers, then load the kernel data segments.
; On entry the stack holds [int_no, err_code, eip, cs, eflags, ...].
%macro SAVE_FRAME 0
    push    ds
    push    es
    push    fs
    push    gs
    pusha
    mov     ax, KERNEL_DS
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
%endmacro

; Undo SAVE_FRAME and drop int_no and err_code.
%macro RESTORE_FRAME 0
    popa
    pop     gs
    pop     fs
    pop     es
    pop     ds
    add     esp, 8
%endmacro
