; src/Group_14/src/paging_asm.asm
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
; Loads a new page directory and enables paging.
; Argument page_directory_phys is at [esp + 4]
; ----------------------------------------------------
paging_activate:
    mov eax, [esp + 4]  ; Get physical address of the Page Directory from the stack
    mov cr3, eax        ; Load the physical address into CR3

    ; Enable paging bit (PG - bit 31) in CR0
    mov eax, cr0        ; Read current CR0 value
    or eax, 0x80000000  ; Set the PG bit (bit 31)
    mov cr0, eax        ; Write the modified value back to CR0

    ret                 ; Return to caller