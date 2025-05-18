; ===============================
;  PAGE‑FAULT ISR (isr_pf.asm)
;  -------------------------------
;  Stand‑alone stub, leaves generic exceptions to isr_stubs.asm
; ===============================
section .text
bits 32

; ***** ADD THIS DEFINITION *****
KERNEL_DS      equ 0x10         ; must match your GDT data‑segment and other stubs
; *******************************

global isr14                    ; exposed to IDT setup

; External C function references
extern page_fault_handler       ; void page_fault_handler(isr_frame_t *frame)
extern find_exception_fixup     ; uint32_t find_exception_fixup(uint32_t fault_eip)
extern invoke_kernel_panic_from_isr ; void invoke_kernel_panic_from_isr(void)
extern serial_putc_asm          ; For debug prints

%define KERNEL_CS 0x08          ; Kernel Code Segment Selector (adjust if different in your GDT)

isr14:
    ; CPU Pushes: [SS_user], [ESP_user], EFLAGS, CS, EIP, ErrorCode (if CPL change or priv violation)

    ; --- Minimal Debug Trace ---
    push eax
    mov  al, 'F'
    call serial_putc_asm
    pop  eax
    ; --- End Debug Trace ---

    push dword 14       ; Push interrupt number (vector 14)

    pusha               ; Save general purpose registers
    push ds             ; Save segment registers
    push es
    push fs
    push gs

    ; Load kernel data segments
    mov ax, KERNEL_DS   ; Use the defined KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- Check if fault occurred in Kernel (CPL=0) or User mode (CPL=3) ---
    ; CS is at [ESP + 60] relative to current ESP after all pushes
    mov ax, word [esp + 60] ; Get CS from saved stack frame
    cmp ax, KERNEL_CS
    jne .user_fault_pf      ; If CS != Kernel CS, handle as user fault

; --- Kernel Mode Page Fault ---
    ; Get faulting EIP: [ESP + 56]
    mov edi, [esp + 56]     ; EDI = faulting EIP
    push edi                ; Pass fault_eip as argument to find_exception_fixup
    call find_exception_fixup
    add  esp, 4             ; Clean up argument
    test eax, eax           ; Check if fixup address was returned in EAX
    jnz  .handle_kernel_fixup ; If non-zero, a fixup exists

; Unhandled kernel page fault -> panic
.kernel_fault_unhandled_pf:
    call invoke_kernel_panic_from_isr ; This C function will call KERNEL_PANIC_HALT
    ; Should not return from panic
    cli
    hlt                     ; Halt if it somehow returns

.handle_kernel_fixup:
    ; EAX contains the fixup_addr. We need to set the EIP in the saved stack frame.
    ; EIP is at [ESP + 56]
    mov [esp + 56], eax     ; Set saved EIP on stack to the fixup_addr
    jmp .restore_and_return_pf

.user_fault_pf:
    ; Pass pointer to the isr_frame_t to the C page fault handler
    mov eax, esp            ; EAX = pointer to the stack frame
    push eax
    call page_fault_handler ; Call C handler: void page_fault_handler(isr_frame_t *frame)
    add  esp, 4             ; Clean up argument

.restore_and_return_pf:
    pop gs                  ; Restore segment registers
    pop fs
    pop es
    pop ds
    popa                    ; Restore general purpose registers
    add esp, 8              ; Pop int_no (14) + ErrorCode (from CPU)
    iret                    ; Return from interrupt
; ===============================
; END OF PAGE-FAULT ISR
; ===============================