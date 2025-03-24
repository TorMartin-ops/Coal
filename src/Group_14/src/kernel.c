#include "libc/stdint.h"
#include "libc/stddef.h"
#include "libc/stdbool.h"
#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include <multiboot2.h>

// Structure for the Multiboot information (as you have).
struct multiboot_info {
    uint32_t size;
    uint32_t reserved;
    struct multiboot_tag *first;
};

// The kernel entry point.
// Called by the bootloader (GRUB/multiboot) with 'magic' and the multiboot info.
void main(uint32_t magic, struct multiboot_info* mb_info_addr) {
    gdt_init();
    idt_init();
    terminal_init();

    terminal_write("Hello, World!\n");
    terminal_write("IDT initialized. Enabling interrupts...\n");

    __asm__ volatile ("sti");

    // Now test software interrupts:
    asm volatile("int $0x0");  // triggers isr0 -> int_handler(0)
    asm volatile("int $0x1");  // triggers isr1 -> int_handler(1)
    asm volatile("int $0x2");  // triggers isr2 -> int_handler(2)

    while (1) {
        __asm__ volatile ("hlt");
    }
}