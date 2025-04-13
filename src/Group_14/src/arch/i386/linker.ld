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

    /* Define end of the kernel image */
    /* This point is after the BSS section */
    . = ALIGN(4K); /* Optional: Align the end symbol */
    end = .;                  /* Standard symbol for the end */
    _kernel_end_phys = .;     /* Physical end address of the kernel */
}