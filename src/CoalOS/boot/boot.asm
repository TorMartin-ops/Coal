; Kernel entry: Multiboot2 (GRUB) and PVH (Firecracker, Cloud Hypervisor, qemu -kernel) both start here in
; 32-bit protected mode with paging off. The code below checks for long mode, builds boot page tables
; (identity map and direct map of the first 4 GiB, kernel image at -2 GiB), switches to 64-bit mode and
; calls main(magic, boot_info_phys) on the kernel stack. Everything in .boot.* is linked at its physical
; address; everything else at KERNEL_VMA (see src/arch/x86_64/linker.ld).

%define KERNEL_VMA    0xFFFFFFFF80000000
%define MB2_MAGIC     0xe85250d6
%define PVH_MAGIC     0x336ec578            ; hvm_start_info.magic ("xEn3")
%define PTE_PRESENT_RW 0x03
%define PDE_BIG_RW    0x83                  ; present, writable, 2 MiB page

%define CR4_PAE       (1 << 5)
%define MSR_EFER      0xC0000080
%define EFER_LME      (1 << 8)
%define EFER_NXE      (1 << 11)
%define CR0_PG_WP     0x80010000

section .boot.header progbits alloc noexec nowrite align=8
mb2_header:
    dd MB2_MAGIC
    dd 0                                    ; architecture: i386 protected mode
    dd mb2_end - mb2_header
    dd -(MB2_MAGIC + (mb2_end - mb2_header))
    dw 0, 0                                 ; end tag
    dd 8
mb2_end:

section .note.Xen note alloc noexec nowrite align=4
    dd 4, 4, 18                             ; namesz, descsz, XEN_ELFNOTE_PHYS32_ENTRY
    db "Xen", 0
    dd pvh_start

section .boot.text progbits alloc exec nowrite align=16
bits 32
global _start, pvh_start

_start:                                     ; Multiboot2: eax = magic, ebx = info
    cli
    mov     [boot_magic], eax
    mov     [boot_info], ebx
    jmp     start32

pvh_start:                                  ; PVH: ebx = struct hvm_start_info
    cli
    mov     dword [boot_magic], PVH_MAGIC
    mov     [boot_info], ebx

start32:
    cld
    mov     esp, boot_stack32_top

    mov     eax, 0x80000000                 ; long mode, NX and PAE are required
    cpuid
    cmp     eax, 0x80000001
    jb      no_long_mode
    mov     eax, 0x80000001
    cpuid
    bt      edx, 29
    jnc     no_long_mode
    bt      edx, 20
    jnc     no_long_mode
    mov     eax, 1
    cpuid
    bt      edx, 6
    jnc     no_long_mode

    ; PD0..PD3: 2 MiB pages for physical 0 .. 4 GiB.
    mov     edi, boot_pd
    mov     eax, PDE_BIG_RW
    mov     ecx, 2048
.fill_pd:
    mov     [edi], eax
    mov     dword [edi + 4], 0
    add     eax, 0x200000
    add     edi, 8
    loop    .fill_pd

    mov     dword [boot_pdpt_lo + 0 * 8], boot_pd + 0x0000 + PTE_PRESENT_RW
    mov     dword [boot_pdpt_lo + 1 * 8], boot_pd + 0x1000 + PTE_PRESENT_RW
    mov     dword [boot_pdpt_lo + 2 * 8], boot_pd + 0x2000 + PTE_PRESENT_RW
    mov     dword [boot_pdpt_lo + 3 * 8], boot_pd + 0x3000 + PTE_PRESENT_RW
    mov     dword [boot_pdpt_hi + 510 * 8], boot_pd + PTE_PRESENT_RW      ; -2 GiB .. -1 GiB = physical 0 .. 1 GiB
    mov     dword [boot_pml4 + 0 * 8], boot_pdpt_lo + PTE_PRESENT_RW      ; identity
    mov     dword [boot_pml4 + 256 * 8], boot_pdpt_lo + PTE_PRESENT_RW    ; direct map (0xFFFF800000000000)
    mov     dword [boot_pml4 + 511 * 8], boot_pdpt_hi + PTE_PRESENT_RW    ; kernel image

    mov     eax, cr4
    or      eax, CR4_PAE
    mov     cr4, eax
    mov     eax, boot_pml4
    mov     cr3, eax
    mov     ecx, MSR_EFER
    rdmsr
    or      eax, EFER_LME | EFER_NXE
    wrmsr
    mov     eax, cr0
    or      eax, CR0_PG_WP
    mov     cr0, eax

    lgdt    [boot_gdt_ptr]
    jmp     0x08:long_mode

no_long_mode:                               ; say so on COM1 and halt
    mov     esi, no_long_msg
    mov     dx, 0x3F8
.put:
    lodsb
    test    al, al
    jz      .halt
    out     dx, al
    jmp     .put
.halt:
    hlt
    jmp     .halt

bits 64
long_mode:
    xor     eax, eax
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rax, kernel_entry
    jmp     rax

section .boot.data progbits alloc noexec write align=16
align 8
boot_gdt:
    dq 0
    dq 0x00209A0000000000                   ; 64-bit code, ring 0
boot_gdt_end:
boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt
no_long_msg: db "Coal OS needs a 64-bit CPU with NX", 13, 10, 0
global boot_magic, boot_info
boot_magic: dd 0
boot_info:  dd 0

section .boot.bss nobits alloc noexec write align=4096
boot_pml4:      resb 4096
boot_pdpt_lo:   resb 4096
boot_pdpt_hi:   resb 4096
boot_pd:        resb 4096 * 4
                resb 512
boot_stack32_top:

section .text
bits 64
default rel
extern main

kernel_entry:
    lea     rsp, [boot_stack_top]
    xor     ebp, ebp
    mov     edi, [abs boot_magic]
    mov     esi, [abs boot_info]
    call    main
.hang:
    cli
    hlt
    jmp     .hang

section .bss align=16
resb 4096 * 4
boot_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
