/* Kernel image: the boot trampoline (.boot.*) runs at its physical address, the rest is linked in the
 * top 2 GiB (mcmodel=kernel) and loaded behind it. */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS {
    boot   PT_LOAD FLAGS(5);
    bootrw PT_LOAD FLAGS(6);
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    note   PT_NOTE;
}

SECTIONS {
    . = 1M;
    _kernel_start_phys = .;

    .boot : {
        KEEP(*(.boot.header))
        *(.boot.text)
    } :boot

    .note : { *(.note.Xen) } :boot :note

    /* Boot variables, page tables and the 32-bit stack; the tables are zero-filled by the loader. */
    .boot.data ALIGN(4096) : { *(.boot.data) } :bootrw
    .boot.bss : { *(.boot.bss) } :bootrw

    . = ALIGN(4096);
    . += KERNEL_VMA;

    _kernel_text_start = .;
    .text : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    _kernel_text_end = .;

    _kernel_rodata_start = .;
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    } :rodata
    /* Fixup table for faults in copy_from_user/copy_to_user. */
    .ex_table : AT(ADDR(.ex_table) - KERNEL_VMA) ALIGN(8) {
        __start_ex_table = .;
        *(.ex_table)
        __stop_ex_table = .;
    } :rodata
    . = ALIGN(4096);
    _kernel_rodata_end = .;

    _kernel_data_start = .;
    .data : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } :data
    .bss : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4096);
    _kernel_data_end = .;
    end = .;
    _kernel_end_phys = . - KERNEL_VMA;

    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
