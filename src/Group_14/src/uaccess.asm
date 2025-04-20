; src/uaccess.asm
; Safe 32-bit user memory access routines using exception tables.

section .text
global _raw_copy_from_user ; Make accessible from C
global _raw_copy_to_user   ; Make accessible from C

; Macro to simplify adding exception table entries
; Usage: EX_TABLE .fault_instruction_label, .fixup_label
; IMPORTANT: This requires your assembler/linker setup to correctly place
;            the .ex_table section and resolve labels. Check NASM docs
;            for section directives if this specific syntax doesn't work.
%macro EX_TABLE 2
    section .ex_table align=4   ; Switch to exception table section
    dd %1                       ; Define 32-bit faulting address (label)
    dd %2                       ; Define 32-bit fixup address (label)
    section .text               ; Switch back to text section
%endmacro


; 32-bit NASM: Standard C calling convention (args on stack)
; Takes: [ebp+8] = kernel dest (EDI), [ebp+12] = user src (ESI), [ebp+16] = count (ECX)
; Returns: EAX = number of bytes *not* copied (0 on success)
_raw_copy_from_user:
    push ebp
    mov ebp, esp
    push esi            ; Save caller's ESI
    push edi            ; Save caller's EDI
    push ecx            ; Save caller's ECX (though we use it directly)

    ; Load arguments into registers
    mov edi, [ebp + 8]  ; edi = k_dst
    mov esi, [ebp + 12] ; esi = u_src
    mov ecx, [ebp + 16] ; ecx = count
    xor eax, eax        ; eax = return value (bytes not copied), initially 0

.loop:
    cmp ecx, 0
    je .done            ; If count is 0, finished successfully

    ; --- Define Exception Table Entry for the next instruction ---
    EX_TABLE .fault_instruction_from, .fault_handler_from

.fault_instruction_from:
    ; Attempt to read 1 byte from user space address in ESI
    mov bl, [esi]       ; <<< THIS INSTRUCTION CAN FAULT >>>
    ; If we get here, the read succeeded
    mov [edi], bl       ; Write the byte to kernel space address in EDI

    ; Move to next byte
    inc edi
    inc esi
    dec ecx
    jmp .loop           ; Continue loop

.fault_handler_from:
    ; We arrive here *only* if the page fault handler modified EIP
    ; because a fault occurred at .fault_instruction_from.
    ; The page fault handler should have put the *remaining* count (current ECX)
    ; into the saved EAX slot on the stack before iret'ing here.
    ; We just need to return that value.
    ; NOTE: We assume the PF handler sets the return value (saved EAX).
    ; If not, we can 'mov eax, ecx' here, but modifying saved state in PF is cleaner.
    jmp .cleanup        ; Jump to cleanup code

.done:
    ; Successfully copied all bytes. EAX is already 0.
    ; Fall through to cleanup.

.cleanup:
    ; Restore saved registers
    pop ecx
    pop edi
    pop esi
    mov esp, ebp        ; Restore stack pointer
    pop ebp
    ret                 ; Return EAX (0 on success, non-zero uncopied bytes on fault)


; 32-bit NASM: Standard C calling convention
; Takes: [ebp+8] = user dest (EDI), [ebp+12] = kernel src (ESI), [ebp+16] = count (ECX)
; Returns: EAX = number of bytes *not* copied (0 on success)
_raw_copy_to_user:
    push ebp
    mov ebp, esp
    push esi
    push edi
    push ecx

    ; Load arguments
    mov edi, [ebp + 8]  ; edi = u_dst
    mov esi, [ebp + 12] ; esi = k_src
    mov ecx, [ebp + 16] ; ecx = count
    xor eax, eax        ; eax = return value (bytes not copied), initially 0

.loop:
    cmp ecx, 0
    je .done

    ; --- Define Exception Table Entry for the next instruction ---
    EX_TABLE .fault_instruction_to, .fault_handler_to

.fault_instruction_to:
    ; Read 1 byte from kernel space address in ESI (should be safe)
    mov bl, [esi]
    ; Attempt to write 1 byte to user space address in EDI
    mov [edi], bl       ; <<< THIS INSTRUCTION CAN FAULT >>>

    ; Move to next byte
    inc edi
    inc esi
    dec ecx
    jmp .loop

.fault_handler_to:
    ; Arrived here via modified IRET from page fault handler.
    ; Assume PF handler placed remaining count (ECX) into saved EAX.
    jmp .cleanup

.done:
    ; Success. EAX is 0.
    ; Fall through to cleanup.

.cleanup:
    pop ecx
    pop edi
    pop esi
    mov esp, ebp
    pop ebp
    ret                 ; Return EAX