; context_switch.asm (Corrected for KERNEL-TO-KERNEL switch return)

global context_switch
section .text

context_switch:
    ; --- Save Old Context ---
    push ebp
    mov ebp, esp
    pushad      ; Save EDI, ESI, (old EBP), ESP_temp, EBX, EDX, ECX, EAX
    pushfd      ; Save EFLAGS
    push gs     ; Save segment registers
    push fs
    push es
    push ds

    ; --- Arguments ---
    ; [ebp+8]  = Arg 1: old_esp_ptr (uint32_t**)
    ; [ebp+12] = Arg 2: new_esp (uint32_t*)
    ; [ebp+16] = Arg 3: new_page_directory (uint32_t*)

    mov eax, [ebp + 8]      ; EAX = old_esp_ptr
    test eax, eax
    jz .skip_esp_save
    mov [eax], esp          ; Save current kernel ESP into *old_esp_ptr
.skip_esp_save:

    ; --- Switch CR3 ---
    mov eax, [ebp + 16]     ; EAX = new_page_directory
    mov ecx, cr3
    cmp eax, ecx
    je .skip_cr3_load
    mov cr3, eax            ; Load new page directory
.skip_cr3_load:

    ; --- Switch Kernel Stack ---
    mov esp, [ebp + 12]     ; Load new kernel ESP

    ; --- Restore New Context (Kernel Registers) ---
    pop ds
    pop es
    pop fs
    pop gs
    popfd                   ; Restore EFLAGS
    popad                   ; Restore general purpose registers

    ; --- Correct Return Sequence for KERNEL switch ---
    ; ESP now correctly points just above the saved EBP on the new stack.
    ; DO NOT DO 'mov esp, ebp'.
    pop ebp                 ; Restore the new task's EBP from its stack.
    ret                     ; Return to the C caller (schedule) using the new stack.