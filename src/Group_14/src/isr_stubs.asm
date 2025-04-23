; Corrected src/isr_stubs.asm
; Defines Interrupt Service Routine (ISR) stubs for exceptions 0-19.
; Uses a common stub to ensure correct stack frame for the C handler.
; ISR 14 (Page Fault) is now handled entirely by isr_pf.asm
; <<< MODIFIED: Added debug print to common_interrupt_stub >>>

section .text

; External C handler function
extern isr_common_handler ; Make sure C code defines this function
extern serial_putc_asm    ; External ASM function for serial output

; Export ISR symbols for use in IDT setup
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8
global isr10, isr11, isr12, isr13 ; isr14 is NOT exported from here
global isr16, isr17, isr18, isr19
; Add isr9, isr15, etc. if needed

; Define Kernel Data Segment selector (Must match irq_stubs.asm and gdt.c)
KERNEL_DATA_SEG equ 0x10

; Common macro for ISRs WITHOUT an error code pushed by CPU
; We push a dummy error code 0.
%macro ISR_NOERRCODE 1
isr%1:
    ; cli          ; Optional: Disable interrupts on entry
    push dword 0   ; Push dummy error code
    push dword %1  ; Push vector number
    jmp common_interrupt_stub
%endmacro

; Common macro for ISRs WITH an error code pushed by the CPU
%macro ISR_ERRCODE 1
isr%1:
    ; cli          ; Optional: Disable interrupts on entry
    ; Error code is already pushed by CPU
    push dword %1  ; Push vector number
    jmp common_interrupt_stub
%endmacro

; Define ISRs using the macros
ISR_NOERRCODE 0    ; Divide By Zero Exception
ISR_NOERRCODE 1    ; Debug Exception
ISR_NOERRCODE 2    ; Non Maskable Interrupt Exception
ISR_NOERRCODE 3    ; Breakpoint Exception
ISR_NOERRCODE 4    ; Into Detected Overflow Exception
ISR_NOERRCODE 5    ; Out of Bounds Exception
ISR_NOERRCODE 6    ; Invalid Opcode Exception
ISR_NOERRCODE 7    ; No Coprocessor Exception
ISR_ERRCODE   8    ; Double Fault (err code) - Prints '8' internally now
; ISR 9 is reserved or might need a stub
; ISR_NOERRCODE 9
ISR_ERRCODE   10   ; Invalid TSS Exception (err code)
ISR_ERRCODE   11   ; Segment Not Present Exception (err code)
ISR_ERRCODE   12   ; Stack Fault Exception (err code)
; --- Modified ISR 13 ---
isr13: ; General Protection Fault (err code)
    ; --- DEBUG PRINT: Unique GP Fault marker ---
    pusha           ; Save registers temporarily
    mov al, 'G'     ; Print 'G'
    call serial_putc_asm
    mov al, 'P'     ; Print 'P'
    call serial_putc_asm
    mov al, '!'     ; Print '!'
    call serial_putc_asm
    popa            ; Restore registers
    ; --- END DEBUG PRINT ---

    ; Original stub logic resumes here:
    ; cli          ; Optional: Disable interrupts on entry
    ; Error code is already pushed by CPU
    push dword 13  ; Push vector number
    jmp common_interrupt_stub
; --- End Modified ISR 13 ---

; ... rest of file ...
; ISR_ERRCODE   14   ; <<< THIS LINE IS REMOVED / COMMENTED OUT >>>
; ISR 15 is reserved or might need a stub
; ISR_NOERRCODE 15
ISR_NOERRCODE 16   ; Floating Point Exception
ISR_ERRCODE   17   ; Alignment Check Exception (err code)
ISR_ERRCODE   18   ; Machine Check Exception (err code) - Note: MCE is complex
ISR_NOERRCODE 19   ; SIMD Floating-Point Exception
; Define stubs for reserved exceptions 20-31 if desired


; Common stub called by all ISRs and IRQs after pushing vector and error code.
; Creates the stack frame expected by the C isr_common_handler(isr_frame_t* frame).
common_interrupt_stub:
    ; --- DEBUG PRINT: Indicate entry into common stub ---
    pusha           ; Save registers temporarily
    mov al, '@'     ; '@' signifies entry to common stub
    call serial_putc_asm
    popa            ; Restore registers
    ; --- END DEBUG PRINT ---

    ; 1. Save segment registers (DS, ES, FS, GS) first, as isr_frame_t expects them
    push ds
    push es
    push fs
    push gs

    ; 2. Save all general purpose registers
    pusha          ; Pushes EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX

    ; 3. Load kernel data segments into segment registers for C handler execution
    mov ax, KERNEL_DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax     ; GS is now set (original was saved by initial push gs)

    ; 4. Call the C handler, passing a pointer to the saved state.
    mov eax, esp   ; Get pointer to the start of the isr_frame_t structure
    push eax       ; Push pointer as argument for isr_common_handler
    call isr_common_handler
    add esp, 4     ; Clean up argument stack

    ; 5. Restore segment registers (placeholder pop gs, real restore is later)
    ;    This pop gs is technically unnecessary as popa doesn't save/restore GS,
    ;    and we restore the original GS manually later. Let's remove it for clarity.
    ;    pop gs <-- REMOVED

    ; 6. Restore general purpose registers
    popa

    ; 7. Restore original segment registers (popped in reverse order of push)
    pop gs ; <<< CORRECTED: Restore original GS here
    pop fs
    pop es
    pop ds

    ; 8. Clean up the vector number and error code pushed by the specific stub
    add esp, 8

    ; 9. Return from interrupt
    ; sti
    iret