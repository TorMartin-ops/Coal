/* Linker script for static x86_64 user programs: code and read-only data in one
 * R-X segment, data and bss in one RW- segment on their own pages, so no page
 * is both writable and executable. */
ENTRY(_start)

PHDRS {
    text  PT_LOAD FLAGS(5);     /* R-X */
    data  PT_LOAD FLAGS(6);     /* RW- */
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text .text.* .gnu.linkonce.t.*)
    } :text

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } :text

    /* A fresh virtual page for the data, at the same offset into its page as
     * in the file: the file needs no padding. */
    . = ALIGN(0x1000) + (. & 0xfff);

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
