section .text
global _raw_copy_from_user ; Make accessible from C
global _raw_copy_to_user   ; Make accessible from C

; Macro to simplify adding exception table entries
; Usage: EX_TABLE .fault_instruction_label, .fixup_label
%macro EX_TABLE 2
    section .ex_table align=4   ; Switch to exception table section
    dd %1                       ; Define 32-bit faulting address (label)
    dd %2                       ; Define 32-bit fixup address (label)
    section .text               ; Switch back to text section
%endmacro


; _raw_copy_from_user
; Copies 'n' bytes from user space (ESI) to kernel space (EDI).
; Handles page faults using exception table.
; Uses optimized word/byte copies.
; Args: [ebp+8] = kernel dest (EDI), [ebp+12] = user src (ESI), [ebp+16] = count (ECX)
; Returns: EAX = number of bytes *not* copied (0 on success)
_raw_copy_from_user:
    push ebp
    mov ebp, esp
    push esi            ; Save caller's ESI
    push edi            ; Save caller's EDI
    push ecx            ; Save original count
    push ebx            ; Save EBX (used for byte moves)

    ; Load arguments into registers
    mov edi, [ebp + 8]  ; edi = k_dst
    mov esi, [ebp + 12] ; esi = u_src
    mov ecx, [ebp + 16] ; ecx = count
    mov eax, ecx        ; Store original count in EAX temporarily for calculation on fault

    ; Optimized copy using dwords (4 bytes) first
.dword_loop:
    cmp ecx, 4
    jl .byte_loop       ; Less than 4 bytes remaining, switch to byte copy

    ; --- Exception Table Entry for movsd ---
    EX_TABLE .fault_movsd_from, .fault_handler_from

.fault_movsd_from:
    movsd               ; Copy 4 bytes from [esi] to [edi], inc esi/edi by 4
                        ; <<< THIS INSTRUCTION CAN FAULT >>>
    sub ecx, 4          ; Decrement count by 4
    jmp .dword_loop     ; Continue dword loop

    ; Copy remaining bytes (0 to 3)
.byte_loop:
    cmp ecx, 0
    je .done            ; If count is 0, finished successfully

    ; --- Exception Table Entry for movsb ---
    EX_TABLE .fault_movsb_from, .fault_handler_from

.fault_movsb_from:
    ; Use movsb to copy 1 byte (requires count in ECX)
    ; Need to reload ECX with 1 for movsb, but keep original remainder safe
    push ecx            ; Save remaining byte count (<4)
    mov ecx, 1          ; Set count for movsb
    movsb               ; Copy 1 byte from [esi] to [edi], inc esi/edi by 1
                        ; <<< THIS INSTRUCTION CAN FAULT >>>
    pop ecx             ; Restore remaining byte count
    dec ecx             ; Decrement remaining count
    jmp .byte_loop      ; Continue byte loop (will eventually hit .done)


.fault_handler_from:
    ; We arrive here *only* if the page fault handler modified EIP
    ; because a fault occurred at .fault_movsd_from or .fault_movsb_from.
    ; Calculate bytes *not* copied. ECX holds the count *remaining* just before the fault.
    ; The PF handler should *not* modify registers like ECX.
    ; EAX should contain the number of *unprocessed* bytes (the current value of ECX).
    mov eax, ecx
    jmp .cleanup        ; Jump to cleanup code

.done:
    ; Successfully copied all bytes.
    xor eax, eax        ; Set return value to 0 (no bytes remaining)
    ; Fall through to cleanup.

.cleanup:
    ; Restore saved registers
    pop ebx
    pop ecx
    pop edi
    pop esi
    mov esp, ebp        ; Restore stack pointer
    pop ebp
    ret                 ; Return EAX (0 on success, non-zero uncopied bytes on fault)


; _raw_copy_to_user
; Copies 'n' bytes from kernel space (ESI) to user space (EDI).
; Handles page faults using exception table.
; Uses optimized word/byte copies.
; Args: [ebp+8] = user dest (EDI), [ebp+12] = kernel src (ESI), [ebp+16] = count (ECX)
; Returns: EAX = number of bytes *not* copied (0 on success)
_raw_copy_to_user:
    push ebp
    mov ebp, esp
    push esi            ; Save caller's ESI
    push edi            ; Save caller's EDI
    push ecx            ; Save original count
    push ebx            ; Save EBX (used for byte moves)

    ; Load arguments
    mov edi, [ebp + 8]  ; edi = u_dst
    mov esi, [ebp + 12] ; esi = k_src
    mov ecx, [ebp + 16] ; ecx = count
    mov eax, ecx        ; Store original count in EAX temporarily

    ; Optimized copy using dwords
.dword_loop:
    cmp ecx, 4
    jl .byte_loop       ; Less than 4 bytes left

    ; --- Exception Table Entry for movsd ---
    EX_TABLE .fault_movsd_to, .fault_handler_to

.fault_movsd_to:
    ; Kernel read [esi] should be safe. User write [edi] can fault.
    movsd               ; Copy 4 bytes from [esi] to [edi], inc esi/edi by 4
                        ; <<< USER WRITE CAN FAULT >>>
    sub ecx, 4
    jmp .dword_loop

    ; Copy remaining bytes
.byte_loop:
    cmp ecx, 0
    je .done

    ; --- Exception Table Entry for movsb ---
    EX_TABLE .fault_movsb_to, .fault_handler_to

.fault_movsb_to:
    push ecx
    mov ecx, 1
    ; Kernel read [esi] safe. User write [edi] can fault.
    movsb               ; Copy 1 byte from [esi] to [edi], inc esi/edi by 1
                        ; <<< USER WRITE CAN FAULT >>>
    pop ecx
    dec ecx
    jmp .byte_loop

.fault_handler_to:
    ; Arrived here via modified IRET from page fault handler.
    ; ECX holds remaining count. Return this value in EAX.
    mov eax, ecx
    jmp .cleanup

.done:
    ; Success.
    xor eax, eax        ; Set return value EAX = 0
    ; Fall through to cleanup.

.cleanup:
    pop ebx
    pop ecx
    pop edi
    pop esi
    mov esp, ebp
    pop ebp
    ret                 ; Return EAX