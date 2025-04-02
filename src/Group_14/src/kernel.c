/**
 * kernel.c
 *
 * World‑Class Upgraded Kernel Entry Point for a 32‑bit x86 OS.
 *
 * This kernel initializes:
 *   - Terminal (for debug output),
 *   - GDT & TSS (for segmentation and safe kernel stack),
 *   - IDT & PIC (for interrupt handling),
 *   - Paging (with identity mapping for low memory and higher‑half mapping for kernel),
 *   - Memory allocators (buddy, per‑CPU, global kmalloc),
 *   - PIT (for timer interrupts),
 *   - Keyboard (with keymap loading and event callbacks),
 *   - Syscall interface (via INT 0x80),
 *   - Process management (ELF loading, process creation, and scheduler integration),
 *   - PC Speaker (for audio feedback),
 *   - Demo usage of dynamic memory allocation.
 *
 * After initialization, interrupts are enabled and the kernel enters an idle loop.
 *
 * Disclaimer:
 *   In production, you must protect shared structures (e.g., with spinlocks)
 *   and perform more robust user memory and privilege checks.
 */

 #include "types.h"
 #include <string.h>
 #include <multiboot2.h>  // Ensure correct path to multiboot2.h
 
 #include "terminal.h"
 #include "gdt.h"
 #include "tss.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "keymap.h"
 #include "buddy.h"
 #include "percpu_alloc.h"
 #include "kmalloc.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "paging.h"
 #include "elf_loader.h"
 #include "process.h"
 #include "syscall.h"
 #include "scheduler.h"
 #include "get_cpu_id.h"
 
 // Multiboot2 magic number expected from the bootloader.
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // Define a single global kernel stack (for uni‑processor or demo purposes).
 #define KERNEL_STACK_SIZE (4096 * 4)
 static __attribute__((aligned(16))) uint8_t kernel_stack[KERNEL_STACK_SIZE];
 
 // 'end' symbol is provided by the linker script, marking the end of kernel image.
 extern uint32_t end;
 
 /**
  * print_hex
  *
  * Prints a 32-bit value in hexadecimal format to the terminal.
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
  * print_memory_layout
  *
  * Displays key kernel memory layout information (e.g., the 'end' symbol)
  * to aid in debugging and system verification.
  */
 static void print_memory_layout(void) {
     terminal_write("\n[Kernel] Memory Layout:\n  - Kernel end address: 0x");
     print_hex((uint32_t)&end);
     terminal_write("\n");
 }
 
 /**
  * init_paging
  *
  * Sets up basic paging with both identity mapping for physical addresses [0..64MB)
  * and a higher‑half mapping starting at 0xC0000000.
  *
  * This demo maps 64MB of physical memory. In production, you might dynamically
  * set up mappings based on available memory and more complex policies.
  */
 static void init_paging(void) {
     const uint32_t PHYS_MAPPING_SIZE = 64 * 1024 * 1024; // 64 MB
     static uint32_t page_directory[1024] __attribute__((aligned(4096)));
 
     // Zero the page directory
     memset(page_directory, 0, sizeof(page_directory));
 
     uint32_t num_pages = PHYS_MAPPING_SIZE / 4096;
 
     // 1) Identity mapping (virtual == physical)
     for (uint32_t i = 0; i < num_pages; i++) {
         uint32_t phys_addr   = i * 4096;
         uint32_t dir_index   = phys_addr >> 22;
         uint32_t table_index = (phys_addr >> 12) & 0x3FF;
 
         if (!(page_directory[dir_index] & PAGE_PRESENT)) {
             uint32_t *pt = (uint32_t *)buddy_alloc(4096);
             if (!pt) {
                 terminal_write("[Paging] Failed to allocate page table (identity mapping).\n");
                 return;
             }
             memset(pt, 0, 4096);
             // Present + Read/Write
             page_directory[dir_index] = (uint32_t)pt | (PAGE_PRESENT | PAGE_RW);
         }
         uint32_t *pt = (uint32_t *)(page_directory[dir_index] & ~0xFFF);
         pt[table_index] = phys_addr | (PAGE_PRESENT | PAGE_RW);
     }
 
     // 2) Higher-half mapping: Map physical addresses [0, 64MB) at 0xC0000000+
     for (uint32_t i = 0; i < num_pages; i++) {
         uint32_t phys_addr   = i * 4096;
         uint32_t virt_addr   = 0xC0000000 + phys_addr;
         uint32_t dir_index   = virt_addr >> 22;
         uint32_t table_index = (virt_addr >> 12) & 0x3FF;
 
         if (!(page_directory[dir_index] & PAGE_PRESENT)) {
             uint32_t *pt = (uint32_t *)buddy_alloc(4096);
             if (!pt) {
                 terminal_write("[Paging] Failed to allocate page table (higher-half mapping).\n");
                 return;
             }
             memset(pt, 0, 4096);
             page_directory[dir_index] = (uint32_t)pt | (PAGE_PRESENT | PAGE_RW);
         }
         uint32_t *pt = (uint32_t *)(page_directory[dir_index] & ~0xFFF);
         pt[table_index] = phys_addr | (PAGE_PRESENT | PAGE_RW);
     }
 
     // Load the page directory into CR3
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
 
     // Enable paging by setting the PG bit (bit 31) in CR0
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000;
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
 
     paging_set_directory(page_directory);
 }
 
 /**
  * main
  *
  * Kernel entry point for a 32-bit Multiboot2 system.
  * Receives the multiboot magic number (in EAX) and pointer to multiboot information (in EBX).
  *
  * This function sequentially initializes all subsystems and then enters an idle loop.
  */
 void main(uint32_t magic, struct multiboot_info *mb_info_addr) {
     // Validate the multiboot magic number
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_init();
         terminal_write("[ERROR] Invalid multiboot2 magic number!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
 
     // 1) Terminal initialization
     terminal_init();
     terminal_write("=== UiAOS: World-Class Kernel Booting ===\n\n");
 
     // 2) GDT & TSS initialization
     terminal_write("[Kernel] Initializing GDT & TSS...\n");
     gdt_init();
     tss_set_kernel_stack((uint32_t)(kernel_stack + KERNEL_STACK_SIZE));
     terminal_write("  [OK] GDT & TSS initialized.\n");
 
     // 3) IDT & PIC initialization
     terminal_write("[Kernel] Initializing IDT & PIC...\n");
     idt_init();
     terminal_write("  [OK] IDT & PIC initialized.\n");
 
     // 4) Paging initialization (both identity and higher-half mappings)
     terminal_write("[Kernel] Setting up Paging...\n");
     init_paging();
     terminal_write("  [OK] Paging enabled.\n");
 
     // 5) Memory Allocators
     terminal_write("[Kernel] Initializing Buddy Allocator...\n");
     // Define a 2MB heap starting at 'end'
     #define BUDDY_HEAP_SIZE (0x200000)
     buddy_init((void *)&end, BUDDY_HEAP_SIZE);
     terminal_write("  [OK] Buddy Allocator initialized.\n");
 
     terminal_write("[Kernel] Initializing Per-CPU Allocator...\n");
     percpu_kmalloc_init();
     terminal_write("  [OK] Per-CPU Allocator initialized.\n");
 
     terminal_write("[Kernel] Initializing Global Kmalloc Allocator...\n");
     kmalloc_init();
     terminal_write("  [OK] Global Kmalloc Allocator initialized.\n");
 
     // 6) Print Kernel Memory Layout
     print_memory_layout();
 
     // 7) PIT Initialization
     terminal_write("[Kernel] Initializing PIT...\n");
     init_pit();
     terminal_write("  [OK] PIT initialized.\n");
 
     // 8) Keyboard Initialization
     terminal_write("[Kernel] Initializing Keyboard...\n");
     keyboard_init();
     terminal_write("  Loading Norwegian keymap...\n");
     keymap_load(KEYMAP_NORWEGIAN);
     keyboard_register_callback(terminal_handle_key_event);
     terminal_write("  [OK] Keyboard initialized.\n");
 
     // 9) Enable interrupts after all critical subsystems are ready.
     terminal_write("[Kernel] Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
 
     // 10) Syscall Interface
     terminal_write("[Kernel] Syscall interface is active.\n");
 
     // 11) Process Management: Create a test user process
     terminal_write("[Kernel] Creating test user process '/kernel.bin'...\n");
     process_t *user_proc = create_user_process("/kernel.bin");
     if (user_proc) {
         add_process_to_scheduler(user_proc);
         terminal_write("  [OK] User process created and scheduled.\n");
     } else {
         terminal_write("  [WARNING] Failed to create user process.\n");
     }
 
     // 12) Demonstrate dynamic memory allocation (per-CPU)
     terminal_write("\n[Kernel] Demonstrating per-CPU allocation...\n");
     int cpu_id = get_cpu_id();
     void *block1 = percpu_kmalloc(1024, cpu_id);
     if (block1) {
         terminal_write("  Allocated 1KB from per-CPU allocator.\n");
     }
     void *block2 = percpu_kmalloc(8192, cpu_id);
     if (block2) {
         terminal_write("  Allocated 8KB (buddy fallback) from per-CPU allocator.\n");
     }
     percpu_kfree(block1, 1024, cpu_id);
     percpu_kfree(block2, 8192, cpu_id);
     terminal_write("  Freed allocated blocks.\n");
 
     // 13) Test Syscall: SYS_WRITE demonstration
     terminal_write("\n[Kernel] Testing SYS_WRITE via syscall interface...\n");
     syscall_context_t ctx;
     ctx.eax = SYS_WRITE;
     ctx.ebx = (uint32_t)"Hello from a test syscall!\n";
     syscall_handler(&ctx);
 
     // 14) PC Speaker: Play a test song
     terminal_write("\n[Kernel] Playing test song...\n");
     play_song(&testSong);
     sleep_interrupt(1500);
 
     // 15) Main idle loop: In a fully multitasking kernel, PIT interrupts would trigger scheduling.
     terminal_write("\n[Kernel] Entering idle loop. System is running...\n");
     while (1) {
         __asm__ volatile("hlt");
     }
 }
 