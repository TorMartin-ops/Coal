/**
 * kernel.c
 * Kernel entry point for a 32-bit x86 OS.
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <multiboot2.h>
 
 #include "terminal.h"
 #include "gdt.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "mem.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // 'end' is defined in the linker script; marks the end of the kernel image.
 extern uint32_t end;
 
 // Minimal multiboot info structure.
 struct multiboot_info {
     uint32_t size;
     uint32_t reserved;
     struct multiboot_tag* first;
 };
 
 // Sets up identity-mapped paging for the first 4 MB.
 static void init_paging(void) {
     static uint32_t page_directory[1024] __attribute__((aligned(4096)));
     static uint32_t first_page_table[1024] __attribute__((aligned(4096)));
 
     for (int i = 0; i < 1024; i++) {
         first_page_table[i] = (i * 0x1000) | 3; // Present, RW
     }
     page_directory[0] = ((uint32_t)first_page_table) | 3;
     for (int i = 1; i < 1024; i++) {
         page_directory[i] = 0;
     }
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Enable paging (PG bit)
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 
 // Helper: Print a 32-bit value in hexadecimal.
 static void print_hex(uint32_t value) {
     char hex[9];
     for (int i = 0; i < 8; i++) {
         uint8_t nibble = (value >> ((7 - i) * 4)) & 0xF;
         hex[i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
     }
     hex[8] = '\0';
     terminal_write(hex);
 }
 
 // Prints the kernel's memory layout.
 static void print_memory_layout(void) {
     terminal_write("Memory Layout:\nKernel end address: ");
     print_hex((uint32_t)&end);
     terminal_write("\n");
 }
 
 // Kernel entry point.
 void main(uint32_t magic, struct multiboot_info* mb_info_addr)
 {
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
 
     terminal_init();
     terminal_write("=== UiAOS Booting ===\n\n");
 
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
 
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
 
     terminal_write("Initializing Kernel Memory...\n");
     init_kernel_memory(&end);
 
     terminal_write("Initializing Paging...\n");
     init_paging();
 
     print_memory_layout();
 
     terminal_write("Initializing PIT...\n");
     init_pit();
 
     terminal_write("Initializing Keyboard...\n");
     init_keyboard();
 
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
 
     terminal_write("Testing ISRs (0..2)...\n");
     asm volatile("int $0x0");
     asm volatile("int $0x1");
     asm volatile("int $0x2");
 
     terminal_write("\nSystem is up. Demonstrating memory usage...\n");
     void* block1 = malloc(1024);
     if (block1) terminal_write("Allocated 1 KB with malloc.\n");
     void* block2 = malloc(4096);
     if (block2) terminal_write("Allocated 4 KB with malloc.\n");
 
     terminal_write("\nPlaying test song via PC speaker...\n");
     play_song(&testSong);
     sleep_interrupt(2000);
 
     terminal_write("Entering main loop.");
     while (1) {
         __asm__ volatile("hlt");
     }
 }
 