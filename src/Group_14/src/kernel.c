/**
 * kernel.c
 * 
 * World–Class Upgraded Kernel Entry Point for a 32-bit x86 OS.
 *
 * This upgraded kernel initializes essential subsystems in the correct order:
 *   - Terminal (for immediate debug output)
 *   - Global Descriptor Table (GDT)
 *   - Interrupt Descriptor Table (IDT) & Programmable Interrupt Controller (PIC)
 *   - Paging (enabled early so that dynamic heap growth works)
 *   - Kernel Memory Manager (using a buddy allocator with dynamic heap extension via paging,
 *     and a unified per-CPU allocator for small allocations)
 *   - Programmable Interval Timer (PIT)
 *   - Keyboard (with dynamic keymap loading and interactive input)
 *   - PC Speaker and Song Player (for audio output)
 *
 * After initialization, the kernel enters the main loop, polling keyboard events.
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <multiboot2.h>
 #include <libc/string.h>
 #include "terminal.h"
 #include "gdt.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "keymap.h"       // For loading keymaps
 #include "buddy.h"        // Buddy allocator interface (for large allocations)
 #include "percpu_alloc.h" // Per-CPU allocator interface for small allocations
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "paging.h"       // Paging interface
 #include "get_cpu_id.h"

 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // 'end' is defined in the linker script; it marks the end of the kernel image.
 extern uint32_t end;
 
 /* Minimal multiboot info structure (unused in this sample) */
 struct multiboot_info {
     uint32_t size;
     uint32_t reserved;
     struct multiboot_tag* first;
 };
 
 /* External function to get the current CPU's ID in an SMP environment.
    This function must be implemented elsewhere (for example, in your CPU/APIC module). */
 extern int get_cpu_id(void);
 
 /**
  * init_paging - Sets up identity-mapped paging for the first 4 MB.
  *
  * This routine creates a simple page directory and one page table,
  * mapping the first 4 MB of memory. It then loads the page directory,
  * enables paging, and sets the global page directory for the paging module.
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
     
     // Load the page directory into CR3.
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set the PG (paging) bit.
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
     
     // Set the global page directory pointer for the paging module.
     paging_set_directory(page_directory);
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
  * Initializes all subsystems in the proper order. Paging is enabled before the
  * memory manager is initialized so that dynamic heap extension (via the buddy allocator)
  * correctly maps new pages. The per-CPU unified allocator is then initialized, which sets up
  * slab caches for small allocations on a per-CPU basis. Finally, the main loop polls for keyboard
  * events.
  */
 void main(uint32_t magic, struct multiboot_info* mb_info_addr) {
     (void)mb_info_addr;  // Unused in this sample.
     
     // Validate multiboot magic.
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
     
     // Initialize the terminal for immediate debug output.
     terminal_init();
     terminal_write("=== UiAOS Booting (Upgraded with Per-CPU Allocator) ===\n\n");
     
     // Initialize Global Descriptor Table.
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
     
     // Initialize Interrupt Descriptor Table & PIC.
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
     
     // Enable Paging BEFORE initializing memory management.
     terminal_write("Initializing Paging...\n");
     init_paging();
     
     // Initialize Buddy Allocator for Kernel Memory.
     // Reserve a 2 MB heap region starting at 'end'.
     terminal_write("Initializing Buddy Allocator...\n");
     #define BUDDY_HEAP_SIZE 0x200000  // 2 MB reserved for buddy allocations
     buddy_init((void*)&end, BUDDY_HEAP_SIZE);
     
     // Initialize Per-CPU Unified Allocator.
     terminal_write("Initializing Per-CPU Unified Allocator...\n");
     percpu_kmalloc_init();
     
     // Display kernel memory layout.
     print_memory_layout();
     
     // Initialize Programmable Interval Timer.
     terminal_write("Initializing PIT...\n");
     init_pit();
     
     // Initialize Keyboard Subsystem.
     terminal_write("Initializing Keyboard...\n");
     keyboard_init();
     
     // Load default keymap (US QWERTY).
     terminal_write("Loading US QWERTY keymap...\n");
     keymap_load(KEYMAP_US_QWERTY);
     
     // Override default keyboard callback with interactive input handler.
     keyboard_register_callback(terminal_handle_key_event);
     
     // Enable interrupts.
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
     
     // Optionally test some interrupts.
     terminal_write("Testing ISRs (0..2)...\n");
     asm volatile("int $0x0");
     asm volatile("int $0x1");
     asm volatile("int $0x2");
     
     // Demonstrate dynamic memory allocation using the per-CPU unified allocator.
     terminal_write("\nSystem is up. Demonstrating per-CPU memory usage...\n");
     int cpu_id = get_cpu_id();  // Get the current CPU's ID.
     void *block1 = percpu_kmalloc(1024, cpu_id);
     if (block1)
         terminal_write("Per-CPU allocator allocated 1 KB.\n");
     void *block2 = percpu_kmalloc(8192, cpu_id);
     if (block2)
         terminal_write("Per-CPU allocator allocated 8 KB (buddy fallback).\n");
     
     // Free the allocated blocks.
     percpu_kfree(block1, 1024, cpu_id);
     percpu_kfree(block2, 8192, cpu_id);
     terminal_write("Per-CPU allocator freed allocated blocks.\n");
     
     // Play a test song via the PC speaker.
     terminal_write("\nPlaying test song via PC speaker...\n");
     play_song(&testSong);
     sleep_interrupt(2000);
     
     // Main loop: poll for keyboard events.
     terminal_write("Entering main loop. Press keys to see echoed characters:\n");
     KeyEvent event;
     while (1) {
         if (keyboard_poll_event(&event)) {
             // Process keyboard events as needed.
         }
         __asm__ volatile("hlt");
     }
 }
 