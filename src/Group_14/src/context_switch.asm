; context_switch.asm (Modified)

global context_switch
section .text

context_switch:
    ; --- Save Old Context ---
    push ebp
    mov ebp, esp
    pushad      ; Save EDI, ESI, (old EBP), ESP_temp, EBX, EDX, ECX, EAX
    pushfd      ; Save EFLAGS
    push gs     ; Save segment registers (optional but good practice)
    push fs
    push es
    push ds

    ; --- Arguments from Stack ---
    ; [ebp+8]  = Arg 1: old_esp_ptr (address where current ESP should be saved) -> uint32_t**
    ; [ebp+12] = Arg 2: new_esp (value for the new kernel ESP) -> uint32_t*
    ; [ebp+16] = Arg 3: new_page_directory (physical address of new page dir) -> uint32_t*

    mov eax, [ebp + 8]      ; EAX = old_esp_ptr (address of the pointer to save ESP to)

    ; *** ADD NULL CHECK HERE ***
    test eax, eax           ; Check if old_esp_ptr (eax) is NULL
    jz .skip_esp_save       ; If it is NULL, jump past the save instruction
    ; *** END NULL CHECK ***

    mov [eax], esp          ; Save current kernel ESP into *old_esp_ptr (e.g., into old_tcb->esp)

.skip_esp_save:             ; Label to jump to if old_esp_ptr was NULL

    ; --- Switch Page Directory (CR3) ---
    mov eax, [ebp + 16]     ; EAX = new_page_directory (physical address)
    mov ecx, cr3            ; ECX = current page directory (for comparison, optional)
    cmp eax, ecx            ; Compare new page directory with current one
    je .skip_cr3_load       ; If they are the same, skip loading CR3 (optimization)

    mov cr3, eax            ; Load new page directory into CR3
.skip_cr3_load:

    ; --- Switch Kernel Stack ---
    mov esp, [ebp + 12]     ; Load new kernel ESP (e.g., from new_tcb->esp)

    ; --- Restore New Context ---
    pop ds                  ; Restore segment registers
    pop es
    pop fs
    pop gs
    popfd                   ; Restore EFLAGS
    popad                   ; Restore general purpose registers (EAX, ECX, EDX, EBX, ESP_temp, old EBP, ESI, EDI)

    ; --- Return ---
    mov esp, ebp            ; Clean up stack frame (or use leave)
    pop ebp
    ret                     ; Return to C caller (schedule function)