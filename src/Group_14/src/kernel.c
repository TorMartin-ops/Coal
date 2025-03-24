/**
 * main.c
 *
 * The primary kernel entry point for a 32-bit x86 OS. 
 * It is called by a Multiboot2-compliant bootloader (e.g., GRUB).
 *
 * Steps:
 *   1) Validate Multiboot magic
 *   2) Initialize a VGA terminal
 *   3) Set up GDT and IDT (and remap PIC)
 *   4) Initialize PIT (timer) on IRQ0
 *   5) Initialize keyboard on IRQ1
 *   6) Enable interrupts
 *   7) Optionally test a few software ISRs
 *   8) Enter a main loop (hlt)
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <multiboot2.h>   // If you're using Multiboot 2
 
 #include "terminal.h"
 #include "gdt.h"
 #include "idt.h"
 #include "pit.h"          // For timer (IRQ0)
 #include "keyboard.h"     // For keyboard (IRQ1)
 
 // Magic number set by a Multiboot2-compliant bootloader
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 /**
  * Minimal multiboot info struct, if needed. 
  * You can expand if you want to parse memory maps, modules, etc.
  */
 struct multiboot_info {
     uint32_t size;
     uint32_t reserved;
     struct multiboot_tag* first;
 };
 
 /**
  * main
  *
  * The kernel entry point, called by your bootloader with:
  *   - 'magic': indicates if the loader is Multiboot2
  *   - 'mb_info_addr': pointer to multiboot info (if you need it)
  */
 void main(uint32_t magic, struct multiboot_info* mb_info_addr)
 {
     // 1) Validate multiboot2 magic
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         // If invalid, initialize terminal to display an error 
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) {
             __asm__ volatile("hlt");
         }
     }
 
     // 2) Initialize VGA terminal
     terminal_init();
     terminal_write("=== UiAOS Booting ===\n\n");
 
     // 3a) GDT setup
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
 
     // 3b) IDT + PIC remap
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
 
     // 4) Timer (PIT) on IRQ0
     terminal_write("Initializing Timer (IRQ0)... ");
     init_timer();
     terminal_write("Done.\n");
 
     // 5) Keyboard on IRQ1
     terminal_write("Initializing Keyboard (IRQ1)... ");
     init_keyboard();
     terminal_write("Done.\n");
 
     // 6) Enable interrupts
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
 
     // 7) (Optional) Test software ISRs 0..2
     terminal_write("Testing software interrupts (ISRs 0..2)...\n");
     asm volatile("int $0x0");  // triggers isr0
     asm volatile("int $0x1");  // triggers isr1
     asm volatile("int $0x2");  // triggers isr2
 
     terminal_write("\nSystem is up. Entering main loop.\n");
 
     // 8) Main kernel loop: 
     //    continuously halt to reduce CPU usage 
     //    until an interrupt occurs (timer, keyboard, etc.)
     while (1) {
         __asm__ volatile("hlt");
     }
 }
 