/* user.ld - Simple linker script for i386 user-space programs */
ENTRY(_start) /* Entry point symbol from entry.asm */

SECTIONS {
    /* Start at a common user-space address, e.g., 0x08048000 */
    . = 0x08048000;

    .text : {
        *(.text .text.* .gnu.linkonce.t.*)
    }

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    }

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : { *(.comment) *(.note*) }
}