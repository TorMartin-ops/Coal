/* user.ld - Simple linker script for UiAOS user-space executables */
ENTRY(_start) /* Define the entry point */

/* Define the memory layout. Load programs somewhere in user space. */
/* Example: start at 16MB virtual address. Adjust as needed. */
START_ADDR = 0x01000000;

SECTIONS
{
    /* Start placing sections at START_ADDR */
    . = START_ADDR;

    /* Code section */
    .text :
    {
        *(.text .text.* .gnu.linkonce.t*)
    }

    /* Read-only data section */
    .rodata :
    {
        *(.rodata .rodata.* .gnu.linkonce.r*)
    }

    /* Read-write data section */
    .data :
    {
        *(.data .data.* .gnu.linkonce.d*)
    }

    /* Uninitialized data section (BSS) */
    /* Ensure it's page aligned if needed by kernel or program */
    .bss :
    {
        *(.bss .bss.* .gnu.linkonce.b*)
        *(COMMON)
    }

    /* Discard standard library sections we don't need/support */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note.gnu.build-id)
    }
}