; -----------------------------------------------------------------------------
; syscall.asm  – INT 0x80 entry/exit stub for UiAOS            (v5.0 2025-05-06)
; -----------------------------------------------------------------------------
;  * Builds the canonical isr_frame_t stack frame expected by syscall_dispatcher
;  * Executes in CPL=0, then returns to CPL=3 with IRET.
;  * Guarantees the user sees:
;        - Correct GP registers (incl. EBX) untouched except for EAX = retval
;        - User   DS/ES/FS/GS = USER_DATA_SELECTOR (0x23) when back in ring 3
;  * Works for both 32-bit GAS (AT&T) or NASM (Intel) syntaxes.  Here we use
;    NASM/Intel because the rest of your tree is NASM.
;
;  On entry (hardware already pushed):            On exit (toward IRET):
;      SS  |            |  <-- only if CPL change     |  (not modified here)
;      ESP |   (lower)  |                             |
;      EFLAGS                                         |
;      CS                                             |
;      EIP  <-- ESP when we start                     |
;  We then build:
;      err_code  (dummy 0)
;      int_no    (0x80)
;      DS,ES,FS,GS  (user values)
;      pusha        (EDI .. EAX)
;  and hand the pointer of EDI to C.
;
;  Selector definitions ------------------------------------------------------
%define KERNEL_DATA_SELECTOR 0x10     ; as in your GDT (ring 0, RW data)
%define USER_DATA_SELECTOR   0x23     ; ring 3, RW data   (index 0x04 | RPL3)

extern  syscall_dispatcher            ; void C-function(isr_frame_t *regs)

section .text
global  syscall_handler_asm

; -----------------------------------------------------------------------------
; INT 0x80 handler
; -----------------------------------------------------------------------------
syscall_handler_asm:
    ; 1) Make room so the stack matches an exception frame with an error-code.
    push    dword 0            ; dummy error code

    ; 2) Push the interrupt/vector number so the C side can tell who called it.
    push    dword 0x80         ; int_no

    ; 3) Save user segment selectors – we **do not** reload them later because
    ;    loading a ring-3 selector in CPL0 and returning with them still active
    ;    triggers #GP on user access (their DPL<-->CPL rules).  Instead we’ll
    ;    discard them and put 0x23 back right before IRET.
    push    ds
    push    es
    push    fs
    push    gs

    ; 4) Save general-purpose registers
    pusha                       ; pushes (EDI,ESI,EBP,ESP-orig,EBX,EDX,ECX,EAX)
    ; At this point ESP -> saved EDI (lowest address inside pusha block)

    ; -------------------------------------------------------------------------
    ; Switch to kernel data segments so C may freely touch memory below 0xC0000000
    mov     ax, KERNEL_DATA_SELECTOR
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    ; -------------------------------------------------------------------------
    ; Call C dispatcher – arg0 = pointer to isr_frame_t (saved edi address)
    mov     eax, esp            ; eax = &regs (isr_frame_t *)
    push    eax
    call    syscall_dispatcher  ; on return: eax = retval, regs->eax set too
    add     esp, 4              ; pop argument

    ; Store return value into the saved EAX slot so popa will restore it.
    mov     [esp + 28], eax     ; 28 = offset of saved EAX inside pusha area

    ; -------------------------------------------------------------------------
    ; Restore caller-saved GP registers
    popa                        ; pops into EAX .. EDI (EAX gets retval)

    ; -------------------------------------------------------------------------
    ; Discard the four user segment dwords we pushed (we will reload fresh ones)
    add     esp, 16             ; skip GS,FS,ES,DS that we saved

    ; Pop int_no + err_code (we do not need them anymore)
    add     esp, 8

    ; -------------------------------------------------------------------------
    ; Load user-mode data selectors so that ring-3 code has valid segments.
    mov     ax, USER_DATA_SELECTOR
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    ; -------------------------------------------------------------------------
    ; Return to the userspace – CPU will pop CS,EIP,EFLAGS,SS,ESP automatically.
    iret
