; paging_asm.asm
; Assembly implementations for paging functions.

section .text

bits 32 ; Specify 32-bit code generation

; Export symbols for the linker
global paging_invalidate_page
global paging_activate

; ----------------------------------------------------
; void paging_invalidate_page(void *vaddr);
; Invalidates the TLB entry for a specific virtual address.
; Argument vaddr is at [esp + 4] (standard C calling convention)
; ----------------------------------------------------
paging_invalidate_page:
    mov eax, [esp + 4]  ; Get vaddr argument from the stack
    invlpg [eax]        ; Invalidate TLB entry for the address pointed to by EAX
    ret                 ; Return to caller

; ----------------------------------------------------
; void paging_activate(uint32_t *page_directory_phys);
; Loads a page directory and enables 4 MiB pages, paging and write protection (CR4.PSE, CR0.PG, CR0.WP).
; Argument page_directory_phys is at [esp + 4]
; ----------------------------------------------------
paging_activate:
    mov eax, [esp + 4]  ; Get physical address of the Page Directory from the stack
    mov cr3, eax        ; Load the physical address into CR3

    mov eax, cr4
    or eax, 0x10        ; PSE: 4 MiB pages for the direct map
    mov cr4, eax

    mov eax, cr0
    or eax, 0x80010000  ; PG (bit 31) | WP (bit 16): the kernel honours read-only pages
    mov cr0, eax        ; Write the modified value back to CR0

    ret                 ; Return to caller
section .note.GNU-stack noalloc noexec nowrite progbits
