; uaccess.asm
; Provides low-level routines for copying data between kernel and user space.
; Includes exception table for page fault handling.
; Version 2.3 - Applied stack imbalance fix for movsb fault paths as per review.

BITS 32

SECTION .text
GLOBAL _raw_copy_from_user
GLOBAL _raw_copy_to_user

%macro EX_TABLE 2
    SECTION .ex_table align=4
    dd %1 ; from_eip
    dd %2 ; to_eip
    SECTION .text
%endmacro

_raw_copy_from_user:
    push ebp
    mov ebp, esp
    push esi
    push edi
    push ebx
    ; ECX (count) is passed directly, not saved here as it's return value / modified

    mov edi, [ebp + 8]  ; k_dst
    mov esi, [ebp + 12] ; u_src
    mov ecx, [ebp + 16] ; n (count)

    test ecx, ecx
    jz .copy_from_done_success  ; If count is 0, nothing to do

.dword_loop_from:
    cmp ecx, 4
    jl .byte_loop_from_check    ; Less than 4 bytes left

    EX_TABLE .movsd_fault_target_from, .fault_handler_movsd_from
.movsd_fault_target_from:
    movsd                       ; Copy 4 bytes: [ESI] -> [EDI], ESI+=4, EDI+=4
    sub ecx, 4
    jmp .dword_loop_from

.byte_loop_from_check:
    test ecx, ecx
    jz .copy_from_done_success  ; If count is 0 after dwords, done

.byte_loop_from:
    ; Byte copy loop:
    push ecx                    ; Save remaining byte count (1, 2, or 3)
    mov ecx, 1                  ; Set count for movsb

    EX_TABLE .movsb_fault_target_from, .fault_handler_movsb_from
.movsb_fault_target_from:
    movsb                       ; Copy 1 byte: [ESI] -> [EDI], ESI++, EDI++

    pop ecx                     ; Restore original remaining byte count
    dec ecx                     ; Decrement count for the byte just copied
    jnz .byte_loop_from         ; Loop if more bytes remaining
    ; If ecx becomes 0, fall through to done_success

.copy_from_done_success:
    xor eax, eax                ; Success: 0 bytes not copied
    jmp .cleanup_from

.fault_handler_movsd_from:
    ; Fault during MOVSD. ECX has bytes remaining at point of fault.
    mov eax, ecx
    jmp .cleanup_from

.fault_handler_movsb_from:
    ; Fault during MOVSB.
    add esp, 4                  ; *** PATCH: Undo the 'push ecx' from .byte_loop_from ***
    mov eax, ecx                ; ECX was 1 (the byte being copied), so 1 byte not copied.
    jmp .cleanup_from

.cleanup_from:
    pop ebx
    pop edi
    pop esi
    mov esp, ebp
    pop ebp
    ret


_raw_copy_to_user:
    push ebp
    mov ebp, esp
    push esi
    push edi
    push ebx
    ; ECX (count) is passed directly

    mov edi, [ebp + 8]  ; u_dst
    mov esi, [ebp + 12] ; k_src
    mov ecx, [ebp + 16] ; n (count)

    test ecx, ecx
    jz .copy_to_done_success

.dword_loop_to:
    cmp ecx, 4
    jl .byte_loop_to_check

    EX_TABLE .movsd_fault_target_to, .fault_handler_movsd_to
.movsd_fault_target_to:
    movsd
    sub ecx, 4
    jmp .dword_loop_to

.byte_loop_to_check:
    test ecx, ecx
    jz .copy_to_done_success

.byte_loop_to:
    push ecx
    mov ecx, 1

    EX_TABLE .movsb_fault_target_to, .fault_handler_movsb_to
.movsb_fault_target_to:
    movsb

    pop ecx
    dec ecx
    jnz .byte_loop_to

.copy_to_done_success:
    xor eax, eax
    jmp .cleanup_to

.fault_handler_movsd_to:
    mov eax, ecx
    jmp .cleanup_to

.fault_handler_movsb_to:
    add esp, 4                  ; *** PATCH: Undo the 'push ecx' from .byte_loop_to ***
    mov eax, ecx                ; ECX was 1.
    jmp .cleanup_to

.cleanup_to:
    pop ebx
    pop edi
    pop esi
    mov esp, ebp
    pop ebp
    ret