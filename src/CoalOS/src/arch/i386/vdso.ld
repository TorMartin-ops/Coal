/* Linker script for the vDSO: one R-X page at VDSO_BASE (include/kernel/cpu/vdso.h) holding the ELF
 * headers, the dynamic symbol tables and the code, which starts at VDSO_VSYSCALL. */
PHDRS {
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);
    dynamic PT_DYNAMIC;
}

SECTIONS {
    . = 0xBFFFF000 + SIZEOF_HEADERS;
    .hash         : { *(.hash) }         :text
    .dynsym       : { *(.dynsym) }       :text
    .dynstr       : { *(.dynstr) }       :text
    .gnu.version   : { *(.gnu.version) }  :text
    .gnu.version_d : { *(.gnu.version_d) } :text
    .dynamic      : { *(.dynamic) }      :text :dynamic

    . = 0xBFFFF400;
    .text : { *(.text.vsyscall) *(.text .text.*) } :text

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.plt*) *(.data*) *(.bss*) *(.rel*) }
}

ASSERT(__kernel_vsyscall == 0xBFFFF400, "__kernel_vsyscall must sit at VDSO_VSYSCALL")
