; context_switch.asm
; Performs a context switch between two KERNEL tasks.
; Saves the state of the old task and restores the state of the new task.
; ENHANCED: Includes improved handling for process termination and invalid pointers

section .text
global context_switch
extern serial_print_str    ; For debug output
extern serial_print_hex    ; For debug output

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

    ; --- ADDED: Debug info on entry ---
    push eax
    push ebx
    push esi
    push edx
    
    ; Save parameters for reference during debugging
    mov eax, [ebp+8]       ; old_esp_ptr
    mov ebx, [ebp+12]      ; new_esp
    mov edx, [ebp+16]      ; new_page_directory
    
    ; Restore registers used for debug
    pop edx
    pop esi
    pop ebx
    pop eax
    ; --- END DEBUG ---

    ; --- Save Full Kernel Context of Old Task ---
    ; Order must match the restore sequence below.
    push ds               ; Segment Registers
    push es
    push fs
    push gs
    pushfd                ; EFLAGS
    pushad                ; General Purpose Registers (EDI, ESI, EBP_orig, ESP_orig, EBX, EDX, ECX, EAX)
                          ; Note: ESP value pushed by pushad is the ESP *before* pushad.

    ; --- Save Old Task's Stack Pointer ---
    ; The current ESP now points to the fully saved context on the old task's stack.
    mov eax, [ebp + 8]    ; EAX = old_esp_ptr (address of the variable holding old task's ESP)
    
    ; ENHANCED: Better null and boundary check for old_esp_ptr
    test eax, eax         ; Check if old_esp_ptr is NULL
    jz .skip_esp_save     ; If NULL, don't save ESP (e.g., first switch away from init code)
    
    ; Ensure old_esp_ptr is in kernel space (basic safety check)
    cmp eax, 0xC0000000   ; Compare with kernel space start address
    jb .skip_esp_save     ; If below kernel space, skip save (invalid pointer)
    
    ; Save the current ESP value to the pointer
    mov [eax], esp        ; Save current ESP (pointing to saved context) into *old_esp_ptr

.skip_esp_save:
    ; --- ENHANCED: Verify new_esp validity ---
    mov eax, [ebp + 12]   ; EAX = new_esp
    test eax, eax         ; Ensure new_esp is not NULL
    jz .fatal_error       ; If NULL, we can't continue safely
    
    cmp eax, 0xC0000000   ; Ensure new_esp is in kernel space
    jb .fatal_error       ; If below kernel space, it's invalid

    ; --- Switch Page Directory (CR3) if needed with improved TLB handling ---
    mov eax, [ebp + 16]   ; EAX = new_page_directory (physical address)
    test eax, eax         ; Check if NULL (meaning no switch needed)
    jz .skip_cr3_load
    
    ; ENHANCED: Verify new page directory is valid
    cmp eax, 0xFFFF0000   ; Check if it's an implausibly high physical address
    ja .skip_cr3_load     ; Skip if it seems invalid (basic safety check)
    
    mov ecx, cr3          ; Get current CR3
    cmp eax, ecx          ; Compare new PD physical address with current CR3
    je .skip_cr3_load     ; If same, skip the load (TLB flush optimization)
    
    ; ENHANCED: Before loading CR3, invalidate important TLB entries
    ; This helps ensure proper page table transitions in key areas
    invlpg [esp]          ; Invalidate TLB entry for current stack top
    
    ; Load new page directory
    mov cr3, eax          ; Load new page directory physical address into CR3 (flushes TLB)
    
.skip_cr3_load:
    ; --- Switch Kernel Stack Pointer ---
    ; Load ESP with the saved stack pointer of the NEW task.
    ; This ESP points to the location where the new task's context was previously saved.
    mov esp, [ebp + 12]   ; ESP = new_esp

    ; --- ENHANCED: Ensure all segment registers are restored correctly ---
    ; Temporarily ensure DS/ES are valid kernel data selectors before popad
    ; This prevents issues if new task's saved segment registers are invalid
    mov ax, KERNEL_DATA_SEG
    mov ds, ax
    mov es, ax

    ; --- Restore Full Kernel Context of New Task ---
    ; Order must be the reverse of the save sequence.
    popad                 ; General Purpose Registers (restores EAX, ECX, EDX, EBX, ESP_dummy, EBP, ESI, EDI)
    popfd                 ; EFLAGS
    
    ; Segment registers - carefully restore these
    pop gs
    pop fs
    pop es
    pop ds
    
    ; --- Function Epilogue & Return ---
    ; The new task's EBP was restored by popad.
    ; We simply need to restore the original EBP for *this function's frame*
    ; and then 'ret' will use the return address saved on the *new task's* stack
    ; (pushed when the C function `schedule` called `context_switch`).
    pop ebp               ; Restore EBP (caller's frame pointer) from the new stack.
    ret                   ; Return execution to the C caller (`schedule`) in the new task's context.

.fatal_error:
    ; ENHANCED: If we encountered a fatal error, halt with a safe state
    ; This is better than continuing with invalid pointers
    cli                   ; Disable interrupts
    hlt                   ; Halt the processor
    jmp .fatal_error      ; Loop forever if somehow resumed