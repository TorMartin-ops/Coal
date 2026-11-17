/* Linker script for the vDSO: one R-X page at VDSO_BASE (include/kernel/cpu/vdso_abi.h) holding the ELF
 * headers, the dynamic symbol tables and the code. */
PHDRS {
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);
    dynamic PT_DYNAMIC;
}

SECTIONS {
    . = 0x7FFFFFFFE000 + SIZEOF_HEADERS;
    .hash         : { *(.hash) }         :text
    .dynsym       : { *(.dynsym) }       :text
    .dynstr       : { *(.dynstr) }       :text
    .gnu.version   : { *(.gnu.version) }  :text
    .gnu.version_d : { *(.gnu.version_d) } :text
    .dynamic      : { *(.dynamic) }      :text :dynamic

    . = ALIGN(16);
    .text : { *(.text .text.*) } :text

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.plt*) *(.data*) *(.bss*) *(.rel*) }
}
