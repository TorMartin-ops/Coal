; jump_user.asm
; Performs the initial jump from Kernel Mode (PL0) to User Mode (PL3)
; using the IRET instruction with a pre-configured stack frame.
; Version: 1.4 (Added HLT after CR3 load for debugging)

section .text
global jump_to_user_mode
extern serial_putc_asm ; For debugging (optional)

; Define user data segment selector (ensure this matches your GDT setup)
%define USER_DATA_SELECTOR 0x23 ; (0x20 | 3)

;-----------------------------------------------------------------------------
; jump_to_user_mode(kernel_stack_ptr, page_directory_phys)
; Args on stack (cdecl):
;   [ebp+8]  = kernel_stack_ptr (uint32_t*) - ESP pointing to the prepared IRET frame on the kernel stack.
;   [ebp+12] = page_directory_phys (uint32_t*) - Physical address of the user process's page directory.
;-----------------------------------------------------------------------------
jump_to_user_mode:
    cli                   ; Disable interrupts during transition

    ; --- Function Prologue ---
    push ebp
    mov ebp, esp

    ; --- Get Arguments ---
    mov edx, [ebp + 8]  ; EDX = kernel_stack_ptr
    mov eax, [ebp + 12] ; EAX = page_directory_phys

    ; --- Switch Page Directory (CR3) ---
    mov ecx, cr3        ; Get current CR3
    cmp eax, ecx
    je .skip_cr3_load
    test eax, eax
    jz .skip_cr3_load
    mov cr3, eax 
     
.skip_cr3_load:                


    ; --- Load User Data Segments ---
    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- Load ESP with pointer to IRET Frame ---
    mov esp, edx        ; ESP now points to the prepared IRET frame

    ; --- DEBUGGING: Halt just before IRET --- <<< Kept original HLT too
    hlt                 ; If it reaches here, segment/ESP loads were OK.


    ; --- Execute IRET ---
    iret

    ; --- Should NEVER Return Here ---
.fail:
    cli
    hlt
    jmp .fail