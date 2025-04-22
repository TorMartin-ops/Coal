; jump_user.asm
; Performs the initial jump from Kernel Mode (PL0) to User Mode (PL3)
; using the IRET instruction with a pre-configured stack frame.
; Version: 1.1 (Added loading of data segment registers)

section .text
global jump_to_user_mode
extern serial_putc_asm ; For debugging (optional)

; Define user data segment selector (ensure this matches your GDT setup)
; GDT Index 4 = User Data, Selector = 4 * 8 = 0x20
; RPL = 3 (User Mode)
%define USER_DATA_SELECTOR 0x23 ; (0x20 | 3)

;-----------------------------------------------------------------------------
; jump_to_user_mode(kernel_stack_ptr, page_directory_phys)
; Args on stack (cdecl):
;   [ebp+8]  = kernel_stack_ptr (uint32_t*) - ESP pointing to the prepared IRET frame on the kernel stack.
;   [ebp+12] = page_directory_phys (uint32_t*) - Physical address of the user process's page directory.
;-----------------------------------------------------------------------------
jump_to_user_mode:
    ; --- Function Prologue ---
    push ebp
    mov ebp, esp

    ; --- Get Arguments ---
    mov edx, [ebp + 8]  ; EDX = kernel_stack_ptr (points to the IRET frame: EIP, CS, EFLAGS, ESP_user, SS_user)
    mov eax, [ebp + 12] ; EAX = page_directory_phys (physical address)

    ; --- Switch Page Directory (CR3) ---
    ; Load the process's page directory. This is essential for user space access.
    mov ecx, cr3        ; Get current CR3 (likely kernel's PD)
    cmp eax, ecx        ; Compare new PD with current
    je .skip_cr3_load   ; Skip if already correct (shouldn't happen on first jump)
    test eax, eax       ; Ensure new PD address is not NULL
    jz .skip_cr3_load   ; Skip if NULL (should not happen)
    mov cr3, eax        ; Load new page directory physical address (flushes TLB)
.skip_cr3_load:

    ; --- Load User Data Segments --- <<< FIX ADDED HERE
    ; Load DS, ES, FS, GS with the User Data Segment Selector before IRET.
    ; This prevents a #GP fault when user code tries to access data.
    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax            ; Load FS/GS as well, typically needed.
    mov gs, ax

    ; --- Load ESP with pointer to IRET Frame ---
    ; The C code prepared the kernel stack starting from the top address downwards.
    ; ESP needs to point to the EIP value on the stack for IRET.
    mov esp, edx        ; ESP now points to the prepared IRET frame

    ; --- Optional Debug Output ---
    ; pusha             ; Save all registers if needed
    ; mov al, 'J'       ; Character 'J' for Jump
    ; call serial_putc_asm
    ; popa              ; Restore registers

    ; --- Execute IRET ---
    ; iret performs the following:
    ; 1. Pops EIP from [ESP]
    ; 2. Pops CS from [ESP+4]
    ; 3. Pops EFLAGS from [ESP+8]
    ; 4. If privilege level changed (PL0 -> PL3):
    ;    a. Pops ESP_user from [ESP+12]
    ;    b. Pops SS_user from [ESP+16]
    ; Atomically loads CS:EIP, SS:ESP, EFLAGS and transfers control.
    iret

    ; --- Should NEVER Return Here ---
    ; If iret fails catastrophically (e.g., triple fault), the system resets.
    ; If it somehow returns (major CPU bug or emulation issue), halt.
.fail:
    cli                 ; Disable interrupts
    hlt                 ; Halt the processor
    jmp .fail           ; Infinite halt loop