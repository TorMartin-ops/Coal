ENTRY(_start)

SECTIONS {
    /* Start assigning addresses from 1MB */
    . = 1M;
    _kernel_start_phys = .;  /* Physical start address of the kernel */

    /* Multiboot header section */
    .boot :
    {
        *(.multiboot_header)
    }

    /* Text (code) section */
    . = ALIGN(4K);
    _kernel_text_start_phys = .; /* Define physical start of .text */
    .text :
    {
        *(.text .text.*)        /* Link all .text sections */
         /* Add other specific text sections if needed, e.g., *(.text.entry) */
    }
    _kernel_text_end_phys = .;   /* Define physical end of .text */

    /* Read-only data section */
    . = ALIGN(4K);
    _kernel_rodata_start_phys = .; /* Define physical start of .rodata */
    .rodata :
    {
        *(.rodata .rodata.*)    /* Link all .rodata sections */
    }
    _kernel_rodata_end_phys = .;   /* Define physical end of .rodata */

    /* Initialized data section */
    . = ALIGN(4K);
    _kernel_data_start_phys = .; /* Define physical start of .data */
    .data :
    {
        *(.data .data.*)        /* Link all .data sections */
    }
    _kernel_data_end_phys = .;   /* Define physical end of .data */

    /* Uninitialized data section (BSS) */
    /* _kernel_data_end_phys already marks the start of BSS conceptually */
    . = ALIGN(4K);
    .bss :
    {
        *(COMMON)              /* Link common symbols */
        *(.bss .bss.*)         /* Link all .bss sections */
    }

    /* Exception table for safe user memory access */
    . = ALIGN(4);
    .ex_table : ALIGN(4) {
        __start_ex_table = .;
        *(.ex_table)
        __stop_ex_table = .;
    }

    /* End of the kernel image. Placed after .ex_table so the heap, which
     * starts here, does not overwrite it. */
    . = ALIGN(4K);
    end = .;
    _kernel_end_phys = .;
}
