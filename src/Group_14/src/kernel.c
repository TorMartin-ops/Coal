/**
 * kernel.c
 *
 * The primary kernel entry point for a 32-bit x86 OS (pure C).
 * Integrates Assignment 4 tasks:
 *   1) Validate Multiboot2 magic
 *   2) Initialize terminal
 *   3) GDT & IDT (and PIC remap)
 *   4) init_kernel_memory(&end) for memory manager
 *   5) init_paging() (placeholder)
 *   6) print_memory_layout() (placeholder)
 *   7) init_timer() (PIT)
 *   8) init_keyboard() (IRQ1)
 *   9) Enable interrupts
 *   10) Test software ISRs (0..2)
 *   11) Demonstrate memory usage (malloc)
 *   12) Main loop (hlt)
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <multiboot2.h>
 
 #include "terminal.h"
 #include "gdt.h"
 #include "idt.h"
 #include "pit.h"       // for init_timer(), sleep functions
 #include "keyboard.h"  // for init_keyboard()
 #include "mem.h"       // for init_kernel_memory, malloc, free
 
 // The magic number set by a Multiboot2-compliant bootloader
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // Extern symbol from linker.ld, marking end of kernel image
 extern uint32_t end;
 
 /*
    Minimal structure for multiboot info. 
    Expand if you want to parse memory maps, modules, etc.
 */
 struct multiboot_info {
     uint32_t size;
     uint32_t reserved;
     struct multiboot_tag* first;
 };
 
 // Placeholder for paging init
 static void init_paging(void)
 {
     // For Assignment 4, this can be a stub or real paging code
     // e.g., set up page directories, enable CR0.PG, etc.
 }
 
 // Placeholder for printing memory layout
 static void print_memory_layout(void)
 {
     terminal_write("Memory Layout (placeholder):\n");
     terminal_write("  The 'end' symbol is at &end.\n");
     // Optionally print address of &end or total memory from multiboot info
 }
 
 /**
  * main
  *
  * Kernel entry point, called by the bootloader with:
  *   - 'magic': indicates if the loader is Multiboot2
  *   - 'mb_info_addr': pointer to the multiboot info (if needed)
  */
 void main(uint32_t magic, struct multiboot_info* mb_info_addr)
 {
     // 1) Validate Multiboot2 magic
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         // If invalid, init terminal and show error
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) {
             __asm__ volatile("hlt");
         }
     }
 
     // 2) Initialize VGA terminal
     terminal_init();
     terminal_write("=== UiAOS Booting (Pure C) ===\n\n");
 
     // 3a) Initialize GDT
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
 
     // 3b) Initialize IDT & Remap PIC
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
 
     // 4) Initialize kernel memory
     terminal_write("Initializing Kernel Memory...\n");
     init_kernel_memory(&end);  // naive bump allocator or other approach
 
     // 5) Initialize paging
     terminal_write("Initializing Paging...\n");
     init_paging();
 
     // 6) Print memory layout
     print_memory_layout();
 
     // 7) Initialize PIT (IRQ0)
     terminal_write("Initializing PIT...\n");
     init_pit();  // from pit.c
 
     // 8) Initialize keyboard (IRQ1)
     terminal_write("Initializing Keyboard...\n");
     init_keyboard();
 
     // 9) Enable interrupts
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
 
     // 10) Test software ISRs 0..2
     terminal_write("Testing software interrupts (ISRs 0..2)...\n");
     asm volatile("int $0x0");  // triggers isr0
     asm volatile("int $0x1");  // triggers isr1
     asm volatile("int $0x2");  // triggers isr2
 
     terminal_write("\nSystem is up. Demonstrating memory usage...\n");
 
     // 11) Demonstrate memory usage with malloc
     void* block1 = malloc(1024);  // allocate 1 KB
     if (block1) {
         terminal_write("Allocated 1 KB with malloc.\n");
     }
     void* block2 = malloc(4096);  // allocate 4 KB
     if (block2) {
         terminal_write("Allocated 4 KB with malloc.\n");
     }
 
     // If you want, free them (though naive bump won't reclaim)
     // free(block1);
     // free(block2);
 
     // 12) Main loop: continuously halt
     terminal_write("Entering main loop. OS dev is fun!\n");
     while (1) {
         __asm__ volatile("hlt");
     }
 }
 