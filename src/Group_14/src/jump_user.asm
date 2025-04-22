section .text
 global jump_to_user_mode
 extern after_cr3_test ; Or remove if not using test anymore
 extern serial_putc_asm

 %define USER_DATA_SELECTOR 0x23

 jump_to_user_mode:
     cli

     push ebp
     mov ebp, esp

     mov edx, [ebp + 8]  ; kernel_stack_ptr
     mov eax, [ebp + 12] ; page_directory_phys

     mov ecx, cr3
     cmp eax, ecx
     je .skip_cr3_load
     test eax, eax
     jz .skip_cr3_load

     and eax, 0xFFFFF000 ; Safety mask CR3 address
     mov cr3, eax        ; Load process page directory physical address

 .skip_cr3_load:

     ; Restore original functionality (remove 'call after_cr3_test' if added)
     mov ax, USER_DATA_SELECTOR
     mov ds, ax
     mov es, ax
     mov fs, ax
     mov gs, ax

     mov esp, edx

     iret

 .fail:
     cli
     hlt
     jmp .fail