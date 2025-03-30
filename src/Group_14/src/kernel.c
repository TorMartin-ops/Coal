/**
 * kernel.c
 * Kernel entry point for a 32-bit x86 OS.
 *
 * This production–quality kernel initializes all essential subsystems:
 * - Global Descriptor Table (GDT)
 * - Interrupt Descriptor Table (IDT) & Programmable Interrupt Controller (PIC)
 * - Kernel Memory and Paging
 * - Programmable Interval Timer (PIT)
 * - Keyboard (with dynamic keymap loading and event polling)
 * - PC Speaker (for audio output)
 * - And other modules (e.g., song player)
 *
 * The main loop polls for keyboard events for additional processing
 * (the keyboard driver’s default callback already echoes key presses).
 *
 * This design emphasizes modularity, robust initialization, and immediate
 * feedback for debugging and production use.
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <multiboot2.h>
 #include <libc/string.h>      // For memset, memcpy
 #include "terminal.h"
 #include "gdt.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "keymap.h"           // For loading keymaps
 #include "mem.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // 'end' is defined in the linker script; marks the end of the kernel image.
 extern uint32_t end;
 
 /* Minimal multiboot info structure. */
 struct multiboot_info {
     uint32_t size;
     uint32_t reserved;
     struct multiboot_tag* first;
 };
 
 /**
  * init_paging - Sets up identity-mapped paging for the first 4 MB.
  */
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
 
 /**
  * print_hex - Helper function to print a 32-bit value in hexadecimal.
  */
 static void print_hex(uint32_t value) {
     char hex[9];
     for (int i = 0; i < 8; i++) {
         uint8_t nibble = (value >> ((7 - i) * 4)) & 0xF;
         hex[i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
     }
     hex[8] = '\0';
     terminal_write(hex);
 }
 
 /**
  * print_memory_layout - Prints the kernel's memory layout.
  */
 static void print_memory_layout(void) {
     terminal_write("Memory Layout:\nKernel end address: ");
     print_hex((uint32_t)&end);
     terminal_write("\n");
 }
 
 /**
  * main - Kernel entry point.
  *
  * Initializes all subsystems and enters the main loop.
  */
 void main(uint32_t magic, struct multiboot_info* mb_info_addr) {
     (void)mb_info_addr;  // Currently unused
 
     // Validate multiboot magic.
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
  
     /* Initialize Terminal first */
     terminal_init();
     terminal_write("=== UiAOS Booting ===\n\n");
  
     /* Initialize Global Descriptor Table */
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
  
     /* Initialize Interrupt Descriptor Table & PIC */
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
  
     /* Initialize Kernel Memory */
     terminal_write("Initializing Kernel Memory...\n");
     init_kernel_memory(&end);
  
     /* Set up Paging */
     terminal_write("Initializing Paging...\n");
     init_paging();
  
     /* Display Memory Layout */
     print_memory_layout();
  
     /* Initialize Programmable Interval Timer */
     terminal_write("Initializing PIT...\n");
     init_pit();
  
     /* Initialize Keyboard Subsystem */
     terminal_write("Initializing Keyboard...\n");
     keyboard_init();
  
     /* Load a default keymap (US QWERTY) */
     terminal_write("Loading US QWERTY keymap...\n");
     keymap_load(KEYMAP_US_QWERTY);
  
     /* Override default keyboard callback with interactive input handler */
     keyboard_register_callback(terminal_handle_key_event);
  
     /* Enable interrupts */
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
  
     /* Optional: Test some ISRs */
     terminal_write("Testing ISRs (0..2)...\n");
     asm volatile("int $0x0");
     asm volatile("int $0x1");
     asm volatile("int $0x2");
  
     /* Demonstrate Memory Allocation */
     terminal_write("\nSystem is up. Demonstrating memory usage...\n");
     void* block1 = malloc(1024);
     if (block1) terminal_write("Allocated 1 KB with malloc.\n");
     void* block2 = malloc(4096);
     if (block2) terminal_write("Allocated 4 KB with malloc.\n");
  
     /* Play a test song using the PC speaker */
     terminal_write("\nPlaying test song via PC speaker...\n");
     play_song(&testSong);
     sleep_interrupt(2000);
  
     /* Main Loop: Poll for keyboard events for additional processing.
        The keyboard driver's callback (now terminal_handle_key_event) manages input.
     */
     terminal_write("Entering main loop. Press keys to see echoed characters:\n");
     KeyEvent event;
     while (1) {
         if (keyboard_poll_event(&event)) {
             // Additional processing can be added here.
         }
         __asm__ volatile("hlt");
     }
 }
 