section .text
     global jump_to_user_mode
     extern serial_putc_asm ; Make sure this is available

     jump_to_user_mode:
         push ebp
         mov ebp, esp

         ; Get arguments
         mov edx, [ebp + 8]  ; EDX = kernel_stack_ptr (new ESP for IRET frame)
         mov eax, [ebp + 12] ; EAX = page_directory_phys (new CR3)

         ; Switch CR3 if necessary
         mov ecx, cr3
         cmp eax, ecx
         je .skip_cr3_load_user
         test eax, eax
         jz .skip_cr3_load_user
         mov cr3, eax
     .skip_cr3_load_user:

         ; Load the new kernel stack pointer. This stack contains the IRET frame.
         mov esp, edx

         ; Memory barrier (optional, likely harmless)
         ; mov eax, cr0
         ; mov eax, cr0

         ; ---> ADDED LOGGING <---
         push eax          ; Save EAX
         mov al, 'J'       ; Character 'J' for Jump
         call serial_putc_asm
         pop eax           ; Restore EAX
         ; ---> END LOGGING <---

         ; Execute IRET to jump to user mode
         iret

     .fail:
         cli
         hlt
         jmp .fail