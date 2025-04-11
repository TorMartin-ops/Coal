ENTRY(_start)

SECTIONS {
    . = 1M;
    _kernel_start_phys = .;  /* Add this line */

    .boot :
    {
        *(.multiboot_header)
    }

    . = ALIGN(4K);
    .text :
    {
        *(.text .text.*)
    }

    . = ALIGN(4K);
    .rodata :
    {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4K);
    .data :
    {
        *(.data .data.*)
    }

    . = ALIGN(4K);
    .bss :
    {
        *(.bss .bss.*)
    }

    end = .;
    _kernel_end_phys = .;
}