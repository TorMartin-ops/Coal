/**
 * kernel.c
 * 
 * World–Class Upgraded Kernel Entry Point for a 32-bit x86 OS.
 *
 * This upgraded kernel initializes essential subsystems in the correct order:
 *   - Terminal (for immediate debug output)
 *   - Global Descriptor Table (GDT) & TSS (for both kernel and user mode)
 *   - Interrupt Descriptor Table (IDT) & Programmable Interrupt Controller (PIC)
 *   - Paging (enabled early so that dynamic heap growth works)
 *   - Kernel Memory Manager (using a buddy allocator with dynamic heap extension via paging,
 *     and a unified per-CPU allocator for small allocations)
 *   - Programmable Interval Timer (PIT)
 *   - Keyboard (with dynamic keymap loading and interactive input)
 *   - PC Speaker and Song Player (for audio output)
 *   - (Future work) User-mode process loader and system call interface.
 *
 * After initialization, the kernel demonstrates dynamic memory allocation using the
 * per-CPU allocator, then enters the main loop to poll for keyboard events.
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
 #include "kmalloc.h"      // Unified kmalloc/kfree interface (can be configured to use per-CPU alloc)
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "paging.h"       // Paging interface
 #include "get_cpu_id.h"
 // Optionally include process and syscall modules when ready
 //#include "process.h"
 //#include "syscall.h"
 
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
    For production, get_cpu_id() is implemented in get_cpu_id.c using CPUID. */
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
         first_page_table[i] = (i * 0x1000) | 3; // Present, RW.
     }
     page_directory[0] = ((uint32_t)first_page_table) | 3;
     for (int i = 1; i < 1024; i++) {
         page_directory[i] = 0;
     }
     
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Enable paging.
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
     
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
  * Initializes subsystems in the proper order. Paging is enabled before the memory manager
  * so that dynamic heap extension works correctly. The buddy allocator is initialized first,
  * followed by the per-CPU unified allocator (kmalloc and percpu_alloc). The kernel then
  * initializes hardware drivers (PIT, keyboard, PC speaker) and enables interrupts. Finally,
  * the kernel demonstrates dynamic memory allocation and enters the main loop.
  */
 void main(uint32_t magic, struct multiboot_info* mb_info_addr) {
     (void)mb_info_addr;  // Unused in this sample.
     
     // Validate multiboot magic.
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_init();
         terminal_write("Error: Invalid multiboot2 magic!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
     
     // Initialize the terminal.
     terminal_init();
     terminal_write("=== UiAOS Booting (Upgraded with User Mode, Syscalls, and Per-CPU Allocator) ===\n\n");
     
     // Initialize GDT (which includes user-mode segments and TSS).
     terminal_write("Initializing GDT... ");
     gdt_init();
     terminal_write("Done.\n");
     
     // Initialize IDT & PIC.
     terminal_write("Initializing IDT & PIC... ");
     idt_init();
     terminal_write("Done.\n");
     
     // Enable paging before memory management.
     terminal_write("Initializing Paging...\n");
     init_paging();
     
     // Initialize the Buddy Allocator with a reserved heap region (e.g., 2 MB starting at 'end').
     terminal_write("Initializing Buddy Allocator...\n");
     #define BUDDY_HEAP_SIZE 0x200000  // 2 MB reserved.
     buddy_init((void*)&end, BUDDY_HEAP_SIZE);
     
     // Initialize the unified per-CPU allocator for small allocations.
     terminal_write("Initializing Per-CPU Unified Allocator...\n");
     percpu_kmalloc_init();
     
     // (Optional) Initialize the global unified allocator if needed.
     kmalloc_init();
     
     // Display memory layout.
     print_memory_layout();
     
     // Initialize the Programmable Interval Timer.
     terminal_write("Initializing PIT...\n");
     init_pit();
     
     // Initialize the Keyboard Subsystem and load the default keymap.
     terminal_write("Initializing Keyboard...\n");
     keyboard_init();
     terminal_write("Loading Norwegian keymap...\n");
     keymap_load(KEYMAP_NORWEGIAN);
     keyboard_register_callback(terminal_handle_key_event);
     
     // Enable interrupts.
     terminal_write("Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
     
     // (Optional) Test some interrupts.
     terminal_write("Testing ISRs (0..2)...\n");
     asm volatile("int $0x0");
     asm volatile("int $0x1");
     asm volatile("int $0x2");
     
     // Demonstrate dynamic memory allocation using the per-CPU allocator.
     terminal_write("\nDemonstrating per-CPU memory usage...\n");
     int cpu_id = get_cpu_id();
     void *block1 = percpu_kmalloc(1024, cpu_id);
     if (block1)
         terminal_write("Per-CPU allocator allocated 1 KB.\n");
     void *block2 = percpu_kmalloc(8192, cpu_id);
     if (block2)
         terminal_write("Per-CPU allocator allocated 8 KB (buddy fallback).\n");
     percpu_kfree(block1, 1024, cpu_id);
     percpu_kfree(block2, 8192, cpu_id);
     terminal_write("Per-CPU allocator freed allocated blocks.\n");
     
     // (Future Integration) Load a user-mode process using your ELF loader and process management.
     // process_t *user_proc = create_user_process("/path/to/user_app.elf");
     // if (user_proc) { add_process_to_scheduler(user_proc); }
     
     // (Future Integration) Set up the system call interface.
     // Ensure that the IDT entry for INT 0x80 is set to your syscall assembly stub.
     
     // Play a test song via the PC speaker.
     terminal_write("\nPlaying test song via PC speaker...\n");
     play_song(&testSong);
     sleep_interrupt(2000);
     
     // Enter main loop: poll for keyboard events.
     terminal_write("Entering main loop. Press keys to see echoed characters:\n");
     KeyEvent event;
     while (1) {
         if (keyboard_poll_event(&event)) {
             // Process keyboard events (user input, commands, etc.)
         }
         __asm__ volatile("hlt");
     }
 }
 