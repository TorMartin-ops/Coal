    section .text
    global jump_to_user_mode  ; Ensure this 'global' directive is present

    ; void jump_to_user_mode(uint32_t *kernel_stack_ptr, uint32_t *page_directory_phys);
    ; Switches CR3 if needed, loads ESP from kernel_stack_ptr, executes IRET.

    jump_to_user_mode:
        push ebp
        mov ebp, esp

        ; Get arguments
        mov edx, [ebp + 8]  ; EDX = kernel_stack_ptr (new ESP)
        mov eax, [ebp + 12] ; EAX = page_directory_phys (new CR3)

        ; Switch CR3 if necessary
        mov ecx, cr3
        cmp eax, ecx
        je .skip_cr3_load_user
        test eax, eax       ; Don't load CR3 if new_pd_phys is NULL (shouldn't happen here, but safe)
        jz .skip_cr3_load_user
        mov cr3, eax        ; Load new page directory (TLB is flushed implicitly)
    .skip_cr3_load_user:

        ; Load the new kernel stack pointer. This stack contains the IRET frame.
        mov esp, edx

        ; Execute IRET to jump to user mode
        ; IRET will pop: EIP, CS, EFLAGS, ESP, SS
        iret

        ; Should never reach here
    .fail:
        cli
        hlt
        jmp .fail

    