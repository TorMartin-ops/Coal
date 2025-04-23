; context_switch.asm - v3 (Simplified - Removed redundant DS/ES load)
; Performs a context switch between two KERNEL tasks.
; Relies solely on stack restore for segments after popad/popfd.

section .text
global context_switch
; extern serial_print_str    ; For debug output (keep if needed)
; extern serial_print_hex    ; For debug output (keep if needed)

; Define Kernel Data and Code Segment selectors (adjust if different in gdt.c)
KERNEL_DATA_SEG equ 0x10
KERNEL_CODE_SEG equ 0x08

;-----------------------------------------------------------------------------
; context_switch(old_esp_ptr, new_esp, new_page_directory)
; Args on stack (cdecl):
;   [ebp+8]  = old_esp_ptr (uint32_t**) - Address where old task's ESP should be saved. NULL if no save needed.
;   [ebp+12] = new_esp (uint32_t*)      - Kernel ESP value for the new task to restore.
;   [ebp+16] = new_page_directory (uint32_t*) - Physical address of new PD, or NULL if no switch needed.
;-----------------------------------------------------------------------------
context_switch:
    ; --- Function Prologue ---
    push ebp
    mov ebp, esp

    ; --- Save Full Kernel Context of Old Task ---
    ; Order must match the restore sequence below.
    push ds               ; Segment Registers
    push es
    push fs
    push gs
    pushfd                ; EFLAGS
    pushad                ; General Purpose Registers (EDI, ESI, EBP_orig, ESP_orig, EBX, EDX, ECX, EAX)

    ; --- Save Old Task's Stack Pointer ---
    mov eax, [ebp + 8]    ; EAX = old_esp_ptr
    test eax, eax
    jz .skip_esp_save
    cmp eax, 0xC0000000   ; Basic check if pointer is in kernel space
    jb .skip_esp_save
    mov [eax], esp        ; Save current ESP (pointing to saved context)

.skip_esp_save:
    ; --- Verify new_esp validity ---
    mov eax, [ebp + 12]   ; EAX = new_esp
    test eax, eax
    jz .fatal_error
    cmp eax, 0xC0000000   ; Basic check if pointer is in kernel space
    jb .fatal_error

    ; --- Switch Page Directory (CR3) if needed ---
    mov eax, [ebp + 16]   ; EAX = new_page_directory
    test eax, eax
    jz .skip_cr3_load
    mov ecx, cr3
    cmp eax, ecx
    je .skip_cr3_load     ; Skip if same PD
    ; Optional: invlpg [esp] could go here if TLB issues suspected, but unlikely cause of #DE
    mov cr3, eax          ; Load new page directory (flushes TLB)

.skip_cr3_load:
    ; --- Switch Kernel Stack Pointer ---
    mov esp, [ebp + 12]   ; ESP = new_esp (Should point to stack frame prepared for idle task)

    ; --- Restore Full Kernel Context of New Task ---
    ; Order must be the reverse of the save sequence.

    ; NOTE: Explicit 'mov ds/es, KERNEL_DATA_SEG' REMOVED. Relying on pops.

    popad                 ; Restores EAX, ECX, EDX, EBX, ESP_dummy, EBP, ESI, EDI
    popfd                 ; Restores EFLAGS
    pop gs                ; Restores GS
    pop fs                ; Restores FS
    pop es                ; Restores ES
    pop ds                ; Restores DS

    ; --- Function Epilogue & Return ---
    pop ebp               ; Restore caller's EBP from the new stack.
    ret                   ; Restore caller's EIP from the new stack & return execution.

.fatal_error:
    ; Safe halt state if invalid new_esp detected
    cli
    hlt
    jmp .fatal_error      ; Loop