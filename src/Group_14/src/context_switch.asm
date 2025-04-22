; context_switch.asm
; Performs a context switch between two KERNEL tasks.
; Saves the state of the old task and restores the state of the new task.

section .text
global context_switch

; Define Kernel Data Segment selector (adjust if different in gdt.c)
KERNEL_DATA_SEG equ 0x10

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
    push ds         ; Segment Registers
    push es
    push fs
    push gs
    pushfd          ; EFLAGS
    pushad          ; General Purpose Registers (EDI, ESI, EBP_orig, ESP_orig, EBX, EDX, ECX, EAX)
                    ; Note: ESP value pushed by pushad is the ESP *before* pushad.

    ; --- Save Old Task's Stack Pointer ---
    ; The current ESP now points to the fully saved context on the old task's stack.
    mov eax, [ebp + 8]      ; EAX = old_esp_ptr (address of the variable holding old task's ESP)
    test eax, eax           ; Check if old_esp_ptr is NULL
    jz .skip_esp_save       ; If NULL, don't save ESP (e.g., first switch away from init code)
    mov [eax], esp          ; Save current ESP (pointing to saved context) into *old_esp_ptr
.skip_esp_save:

    ; --- Switch Page Directory (CR3) if needed ---
    mov eax, [ebp + 16]     ; EAX = new_page_directory (physical address)
    test eax, eax           ; Check if NULL (meaning no switch needed)
    jz .skip_cr3_load
    mov ecx, cr3            ; Get current CR3
    cmp eax, ecx            ; Compare new PD physical address with current CR3
    je .skip_cr3_load       ; If same, skip the load (TLB flush optimization)
    mov cr3, eax            ; Load new page directory physical address into CR3 (flushes TLB)
.skip_cr3_load:

    ; --- Switch Kernel Stack Pointer ---
    ; Load ESP with the saved stack pointer of the NEW task.
    ; This ESP points to the location where the new task's context was previously saved.
    mov esp, [ebp + 12]     ; ESP = new_esp

    ; --- Restore Full Kernel Context of New Task ---
    ; Order must be the reverse of the save sequence.
    popad                   ; General Purpose Registers (restores EAX, ECX, EDX, EBX, ESP_dummy, EBP, ESI, EDI)
    popfd                   ; EFLAGS
    pop gs                  ; Segment Registers
    pop fs
    pop es
    pop ds

    ; --- Function Epilogue & Return ---
    ; The new task's EBP was restored by popad.
    ; We simply need to restore the original EBP for *this function's frame*
    ; and then 'ret' will use the return address saved on the *new task's* stack
    ; (pushed when the C function `schedule` called `context_switch`).
    pop ebp                 ; Restore EBP (caller's frame pointer) from the new stack.
    ret                     ; Return execution to the C caller (`schedule`) in the new task's context.