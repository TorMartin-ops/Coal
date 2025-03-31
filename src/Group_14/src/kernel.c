/**
 * kernel.c
 *
 * World-Class Upgraded Kernel Entry Point for a 32-bit x86 OS.
 *
 * This kernel initializes:
 *   - Terminal (for debug output),
 *   - GDT & TSS (segmentation + safe kernel stack),
 *   - IDT & PIC (interrupt handling),
 *   - Paging (identity + higher-half),
 *   - Memory allocators (buddy, per-CPU, global),
 *   - PIT, Keyboard, PC Speaker,
 *   - Process management (ELF loading),
 *   - Syscalls (INT 0x80),
 * Then enables interrupts and enters a main loop.
 */

 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include <libc/stdbool.h>
 #include <libc/string.h>
 #include <multiboot2.h>  // Make sure this is the correct path to your multiboot2.h
 
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
 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 /*
  * We define a single global kernel stack here.
  * In real SMP systems, each CPU typically has its own stack in the TSS.
  */
 #define KERNEL_STACK_SIZE (4096 * 4)
 /* 
  * Move the alignment attribute to the front or with the type,
  * so older compilers don't complain on arrays.
  */
 static __attribute__((aligned(16))) uint8_t kernel_stack[KERNEL_STACK_SIZE];
 
 /* 
  * Symbol from your linker script marking the end of the kernel image. 
  * Used so the buddy allocator knows where free memory can begin.
  */
 extern uint32_t end;
 
 /**
  * A simple utility to print a 32-bit value in hex.
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
  * Show kernel memory layout: specifically the 'end' symbol address.
  */
 static void print_memory_layout(void) {
     terminal_write("\n[Kernel] Memory Layout:\n  - Kernel end address: 0x");
     print_hex((uint32_t)&end);
     terminal_write("\n");
 }
 
 /**
  * init_paging - Basic identity + higher-half mapping for first 64 MB
  *
  * Physical addresses [0..16MB) are mapped at 0x00000000 (identity)
  * and also at [0xC0000000..(0xC0000000+16MB)] (higher-half).
  *
  * This is a simple example. A real OS might do more sophisticated mappings.
  */
 static void init_paging(void) {
     const uint32_t PHYS_MAPPING_SIZE = 64 * 1024 * 1024; // 64 MB
     static uint32_t page_directory[1024] __attribute__((aligned(4096)));
 
     // Clear PD
     for (int i = 0; i < 1024; i++) {
         page_directory[i] = 0;
     }
 
     uint32_t num_pages = PHYS_MAPPING_SIZE / 4096;
 
     // 1) Identity mapping
     for (uint32_t i = 0; i < num_pages; i++) {
         uint32_t phys_addr   = i * 4096;
         uint32_t dir_index   = phys_addr >> 22;
         uint32_t table_index = (phys_addr >> 12) & 0x3FF;
 
         if (!(page_directory[dir_index] & 1)) { // PAGE_PRESENT = 1
             uint32_t* pt = buddy_alloc(4096);
             if (!pt) {
                 terminal_write("[Paging] Failed to alloc PT (identity)\n");
                 return;
             }
             for (int j = 0; j < 1024; j++)
                 pt[j] = 0;
             // Present + RW
             page_directory[dir_index] = (uint32_t)pt | 3; 
         }
         uint32_t* pt = (uint32_t*)(page_directory[dir_index] & ~0xFFF);
         pt[table_index] = phys_addr | 3; // present + rw
     }
 
     // 2) Higher-half mapping @ 0xC0000000
     for (uint32_t i = 0; i < num_pages; i++) {
         uint32_t phys_addr   = i * 4096;
         uint32_t virt_addr   = 0xC0000000 + phys_addr;
         uint32_t dir_index   = virt_addr >> 22;
         uint32_t table_index = (virt_addr >> 12) & 0x3FF;
 
         if (!(page_directory[dir_index] & 1)) { // PAGE_PRESENT
             uint32_t* pt = buddy_alloc(4096);
             if (!pt) {
                 terminal_write("[Paging] Failed to alloc PT (higher-half)\n");
                 return;
             }
             for (int j = 0; j < 1024; j++)
                 pt[j] = 0;
             page_directory[dir_index] = (uint32_t)pt | 3; 
         }
         uint32_t* pt = (uint32_t*)(page_directory[dir_index] & ~0xFFF);
         pt[table_index] = phys_addr | 3; 
     }
 
     // Load CR3
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
 
     // Enable paging by setting PG bit in CR0
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000;  // set paging bit
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
 
     paging_set_directory(page_directory);
 }
 
 /**
  * main - kernel entry point
  *
  * For a 32-bit Multiboot2 system:
  *   - 'magic' = EAX
  *   - 'mb_info_addr' = EBX
  */
 void main(uint32_t magic, struct multiboot_info *mb_info_addr) {
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         // Minimal terminal to show the error
         terminal_init();
         terminal_write("[ERROR] Invalid multiboot2 magic number!\n");
         while (1) { __asm__ volatile("hlt"); }
     }
 
     // 1) Basic terminal initialization
     terminal_init();
     terminal_write("=== UiAOS: World-Class Kernel Booting ===\n\n");
 
     // 2) Initialize GDT & TSS
     terminal_write("[Kernel] Initializing GDT & TSS...\n");
     gdt_init();
     // Use our dedicated kernel stack when CPU transitions to ring 0
     tss_set_kernel_stack((uint32_t)(kernel_stack + KERNEL_STACK_SIZE));
     terminal_write("  [OK] GDT & TSS ready.\n");
 
     // 3) Initialize IDT & PIC
     terminal_write("[Kernel] Initializing IDT & PIC...\n");
     idt_init();
     terminal_write("  [OK] IDT & PIC ready.\n");
 
     // 4) Paging
     terminal_write("[Kernel] Setting up Paging...\n");
     init_paging();
     terminal_write("  [OK] Paging enabled.\n");
 
     // 5) Memory allocators
     terminal_write("[Kernel] Initializing Buddy Allocator...\n");
     #define BUDDY_HEAP_SIZE (0x200000) // 2 MB from 'end'
     buddy_init((void *)&end, BUDDY_HEAP_SIZE);
 
     terminal_write("[Kernel] Initializing Per-CPU Allocator...\n");
     percpu_kmalloc_init();
 
     terminal_write("[Kernel] Initializing Kmalloc (slab) Allocator...\n");
     kmalloc_init();
 
     // Print kernel memory layout (for demonstration)
     print_memory_layout();
 
     // 6) PIT
     terminal_write("[Kernel] Initializing PIT...\n");
     init_pit();
 
     // 7) Keyboard
     terminal_write("[Kernel] Initializing Keyboard...\n");
     keyboard_init();
     terminal_write("  Loading Norwegian keymap...\n");
     keymap_load(KEYMAP_NORWEGIAN);
     keyboard_register_callback(terminal_handle_key_event);
 
     // 8) Now it is safe to enable interrupts
     terminal_write("[Kernel] Enabling interrupts (STI)...\n");
     __asm__ volatile("sti");
 
     // 9) Syscall interface
     terminal_write("[Kernel] Syscall interface is up.\n");
 
     // 10) Create test user process (ELF loading)
     terminal_write("[Kernel] Attempting to create user process '/kernel.bin'...\n");
     process_t *user_proc = create_user_process("/kernel.bin");
     if (user_proc) {
         add_process_to_scheduler(user_proc);
     } else {
         terminal_write("  [WARNING] Unable to create user process.\n");
     }
 
     // 11) Demonstrate dynamic memory usage
     terminal_write("\n[Kernel] Demonstrating per-CPU allocation...\n");
     int cpu_id = get_cpu_id();
     void *block1 = percpu_kmalloc(1024, cpu_id);
     if (block1) {
         terminal_write("  Allocated 1KB in per-CPU slab.\n");
     }
 
     void *block2 = percpu_kmalloc(8192, cpu_id);
     if (block2) {
         terminal_write("  Allocated 8KB (buddy fallback).\n");
     }
 
     // Free them
     percpu_kfree(block1, 1024, cpu_id);
     percpu_kfree(block2, 8192, cpu_id);
     terminal_write("  Freed both blocks.\n");
 
     // 12) Manually test a syscall
     terminal_write("\n[Kernel] Testing SYS_WRITE via syscall_handler...\n");
     syscall_context_t ctx;
     ctx.eax = SYS_WRITE;
     ctx.ebx = (uint32_t)"Hello from a test syscall!\n";
     syscall_handler(&ctx);
 
     // 13) Test PC speaker
     terminal_write("\n[Kernel] Playing testSong...\n");
     play_song(&testSong);
     sleep_interrupt(1500);
 
     // 14) Final main loop (PIT triggers scheduling if multi–tasking is set up)
     terminal_write("\n[Kernel] Entering idle loop...\n");
     while (1) {
         __asm__ volatile("hlt");
     }
 }