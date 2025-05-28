; context_switch.asm - v3 (Simplified - Removed redundant DS/ES load)
; Performs a context switch between two KERNEL tasks.
; Relies solely on stack restore for segments after popad/popfd.

section .text
global context_switch
extern serial_printf       ; For debug output
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

    ; --- Save Full Kernel Context of Old Task ---
    ; Order must match the restore sequence below.
    
    ; Ensure DS is valid before saving
    push eax
    mov ax, ds
    test ax, ax
    jnz .ds_valid_save
    mov ax, KERNEL_DATA_SEG
    mov ds, ax
.ds_valid_save:
    pop eax
    
    pushad                ; General Purpose Registers (EDI, ESI, EBP_orig, ESP_orig, EBX, EDX, ECX, EAX)
    pushfd                ; EFLAGS
    push gs               ; Segment Registers (in reverse order for correct pop)
    push fs
    push es
    push ds
    
    ; DEBUG: Log what we just saved
    push eax
    push edx
    push ecx
    mov eax, esp
    add eax, 12        ; Adjust for the debug pushes
    push eax
    push debug_save_msg
    call serial_printf
    add esp, 8
    
    ; Print segment registers we just saved
    ; Stack after 3 debug pushes: [esp+0]=ecx, [esp+4]=edx, [esp+8]=eax, [esp+12]=DS, [esp+16]=ES, [esp+20]=FS, [esp+24]=GS
    mov eax, [esp + 12]  ; DS
    push eax
    mov eax, [esp + 16 + 4]  ; ES (adjust for DS push)
    push eax
    mov eax, [esp + 20 + 8]  ; FS (adjust for DS+ES pushes)
    push eax
    mov eax, [esp + 24 + 12]  ; GS (adjust for DS+ES+FS pushes)
    push eax
    push debug_segs_msg
    call serial_printf
    add esp, 20
    
    ; Check if DS is valid
    mov eax, [esp + 12]  ; DS
    test eax, eax
    jnz .ds_ok
    push eax
    push debug_ds_zero_msg
    call serial_printf
    add esp, 8
.ds_ok:
    
    pop ecx
    pop edx
    pop eax

    ; --- Save Old Task's Stack Pointer ---
    mov eax, [ebp + 8]    ; EAX = old_esp_ptr
    test eax, eax         ; Check if old_esp_ptr is NULL
    jz .skip_esp_save     ; If NULL (first switch from scheduler_start), skip saving ESP
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
    
    ; DEBUG: Log what we're about to restore
    push eax
    push edx
    push ecx
    push esp
    push debug_restore_msg
    call serial_printf
    add esp, 8
    
    ; Print segment registers we're about to restore
    ; Stack layout at this point (after push eax, push edx, push ecx):
    ; [ESP+0] = ECX (we pushed)
    ; [ESP+4] = EDX (we pushed)
    ; [ESP+8] = EAX (we pushed)
    ; [ESP+12] = DS (from context)
    ; [ESP+16] = ES
    ; [ESP+20] = FS
    ; [ESP+24] = GS
    ; [ESP+28] = EFLAGS
    ; [ESP+32] = EAX (from pushad)
    ; [ESP+36] = ECX
    ; [ESP+40] = EDX
    ; [ESP+44] = EBX
    ; [ESP+48] = ESP (dummy)
    ; [ESP+52] = EBP
    ; [ESP+56] = ESI
    ; [ESP+60] = EDI
    mov eax, [esp + 24]  ; GS
    push eax
    mov eax, [esp + 20]  ; FS
    push eax
    mov eax, [esp + 16]  ; ES
    push eax
    mov eax, [esp + 12]  ; DS
    push eax
    push debug_segs_msg
    call serial_printf
    add esp, 20
    
    ; Check if DS is valid before restore
    mov eax, [esp + 12]  ; DS
    test eax, eax
    jnz .ds_restore_ok
    push eax
    push debug_ds_restore_zero_msg
    call serial_printf
    add esp, 8
    ; Force DS to kernel data segment
    mov word [esp + 12], KERNEL_DATA_SEG
.ds_restore_ok:
    
    ; Additional check - if ES looks wrong, fix it too
    mov eax, [esp + 16]  ; ES
    cmp eax, 0x100
    jbe .es_restore_ok
    push eax
    push debug_es_corrupt_msg
    call serial_printf
    add esp, 8
    ; Force ES to kernel data segment
    mov word [esp + 16], KERNEL_DATA_SEG
.es_restore_ok:
    
    pop ecx
    pop edx
    pop eax

    ; --- Restore Full Kernel Context of New Task ---
    ; Order must be the reverse of the save sequence.

    ; NOTE: Explicit 'mov ds/es, KERNEL_DATA_SEG' REMOVED. Relying on pops.

    popad                 ; Restores EAX, ECX, EDX, EBX, ESP_dummy, EBP, ESI, EDI
    popfd                 ; Restores EFLAGS
    pop gs                ; Restores GS
    pop fs                ; Restores FS
    pop es                ; Restores ES
    pop ds                ; Restores DS
    
    ; Ensure DS is valid after restore
    push eax
    mov ax, ds
    test ax, ax
    jnz .ds_valid_restore
    mov ax, KERNEL_DATA_SEG
    mov ds, ax
    push edx
    push debug_ds_fixed_msg
    call serial_printf
    add esp, 4
    pop edx
.ds_valid_restore:
    pop eax

    ; --- Function Epilogue & Return ---
    pop ebp               ; Restore caller's EBP from the new stack.
    ret                   ; Restore caller's EIP from the new stack & return execution.

.fatal_error:
    ; Safe halt state if invalid new_esp detected
    cli
    hlt
    jmp .fatal_error      ; Loop

section .rodata
debug_save_msg:    db "[CTX ASM] Saving context at ESP=%p", 10, 0
debug_restore_msg: db "[CTX ASM] Restoring from ESP=%p", 10, 0  
debug_segs_msg:    db "[CTX ASM] Segments: GS=%x FS=%x ES=%x DS=%x", 10, 0
debug_ds_zero_msg: db "[CTX ASM] WARNING: DS is 0 during save!", 10, 0
debug_ds_restore_zero_msg: db "[CTX ASM] WARNING: DS is 0 during restore! Forcing to KERNEL_DATA_SEG", 10, 0
debug_ds_fixed_msg: db "[CTX ASM] Fixed DS=0 after restore", 10, 0
debug_es_corrupt_msg: db "[CTX ASM] WARNING: ES=0x%x looks corrupt! Forcing to KERNEL_DATA_SEG", 10, 0