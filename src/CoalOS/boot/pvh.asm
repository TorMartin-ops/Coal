; PVH entry (Xen's direct boot ABI, also used by QEMU -kernel, Firecracker and Cloud Hypervisor).
; The loader enters in 32-bit protected mode, paging off, with EBX pointing at hvm_start_info.
; pvh_build_info() turns it into a Multiboot2 information block; from there the normal boot path runs.

extern pvh_build_info
extern _start

XEN_ELFNOTE_PHYS32_ENTRY equ 18

section .note.Xen note alloc noexec nowrite align=4
    dd 4                        ; name size
    dd 4                        ; descriptor size
    dd XEN_ELFNOTE_PHYS32_ENTRY
    db "Xen", 0
    dd pvh_start

section .text
bits 32

global pvh_start
pvh_start:
    cli
    cld
    mov esp, pvh_stack_top
    push ebx                    ; struct hvm_start_info *
    call pvh_build_info         ; returns the Multiboot2 info address
    mov ebx, eax
    mov eax, 0x36d76289         ; Multiboot2 bootloader magic
    jmp _start

section .bss
    resb 2048
pvh_stack_top:
section .note.GNU-stack noalloc noexec nowrite progbits
