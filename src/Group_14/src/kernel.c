#include "libc/stdint.h"
#include "libc/stddef.h"
#include "libc/stdbool.h"
#include "terminal.h"
#include "gdt.h"
#include <multiboot2.h>

// Structure representing the Multiboot information
struct multiboot_info {
    uint32_t size;
    uint32_t reserved;
    struct multiboot_tag *first;
};

// Kernel entry point
void main(uint32_t magic, struct multiboot_info* mb_info_addr) {
    gdt_init();         // Initialize GDT
    terminal_init();    // Initialize Terminal

    terminal_write("Hello, World!"); // Print to screen

    while (1) { __asm__("hlt"); } // Halt CPU
}
