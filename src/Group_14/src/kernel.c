/**
 * kernel.c - Main kernel entry point with Multiboot memory parsing.
 */
 #include "types.h"
 #include <string.h>
 #include <multiboot2.h> // Use the standard header
 
 #include "terminal.h"
 #include "gdt.h"
 #include "tss.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "keymap.h"
 #include "buddy.h"
 #include "kmalloc.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "paging.h"     // Provides PAGE_SIZE, KERNEL_SPACE_VIRT_START etc.
 #include "elf_loader.h"
 #include "process.h"
 #include "syscall.h"
 #include "scheduler.h"
 #include "get_cpu_id.h"
 #include "fs_init.h"
 #include "read_file.h"
 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // Minimum heap size requirement in bytes (1MB)
 #define MIN_HEAP_SIZE (1024 * 1024)
 
 // Recommended initial physical memory mapping size (16MB)
 #define INITIAL_PHYS_MAPPING_SIZE (16 * 1024 * 1024)
 
 // Kernel stack (temporary boot stack)
 #define BOOT_STACK_SIZE (4096 * 4)
 static __attribute__((aligned(16))) uint8_t boot_stack[BOOT_STACK_SIZE];
 
 extern uint32_t end; // Provided by linker script
 uintptr_t kernel_image_end_phys = (uintptr_t)&end;
 
 extern uint32_t* kernel_page_directory; // Defined in paging.c
 
 /**
  * @brief Finds a Multiboot tag by type
  * 
  * @param mb_info_phys_addr Physical address of multiboot info structure
  * @param type Tag type to find
  * @return Pointer to tag or NULL if not found
  */
 struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
     // First tag is 8 bytes after the total_size and reserved fields
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     
     // Iterate through tags
     while (tag->type != MULTIBOOT_TAG_TYPE_END) {
         if (tag->type == type) {
             return tag;
         }
         // Move to the next tag: address + size, aligned up to 8 bytes
         tag = (struct multiboot_tag *)((uintptr_t)tag + ((tag->size + 7) & ~7));
     }
     return NULL; // Tag not found
 }
 
 /**
  * @brief Finds the largest available RAM region above 1MB physical address
  * 
  * @param mmap_tag Memory map tag from Multiboot
  * @param out_base_addr Output parameter for base address
  * @param out_size Output parameter for size
  * @return true on success, false if no suitable region found
  */
 bool find_largest_memory_area(struct multiboot_tag_mmap *mmap_tag, uintptr_t *out_base_addr, size_t *out_size) {
     uintptr_t best_base = 0;
     uint64_t best_size = 0; // Use 64-bit for size comparison
 
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
 
     terminal_write("Memory Map (from Multiboot):\n");
     
     // Iterate through all memory map entries
     while ((uintptr_t)mmap_entry < mmap_end) {
         terminal_printf("  Addr: 0x%x%x, Len: 0x%x%x, Type: %d\n",
                       (uint32_t)(mmap_entry->addr >> 32), (uint32_t)mmap_entry->addr,
                       (uint32_t)(mmap_entry->len >> 32), (uint32_t)mmap_entry->len,
                       mmap_entry->type);
 
         // Check if it's available RAM (Type 1) and above 1MB
         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->addr >= 0x100000) {
             // Adjust start address if it overlaps with kernel image end
             uintptr_t entry_phys_start = (uintptr_t)mmap_entry->addr;
             uint64_t entry_len = mmap_entry->len;
             uintptr_t usable_start = entry_phys_start;
 
             // Handle kernel overlap
             if (usable_start < kernel_image_end_phys) {
                 if (entry_phys_start + entry_len > kernel_image_end_phys) {
                     // Region starts below kernel end but extends past it
                     uint64_t overlap = kernel_image_end_phys - usable_start;
                     usable_start = kernel_image_end_phys;
                     entry_len -= overlap;
                 } else {
                     // Region is entirely below kernel end, skip
                     entry_len = 0;
                 }
             }
 
             // Remember best region found
             if (entry_len > best_size) {
                 best_size = entry_len;
                 best_base = usable_start; // Use the potentially adjusted start
             }
         }
 
         // Move to the next entry
         mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
     }
 
     if (best_size > 0) {
         *out_base_addr = best_base;
         *out_size = (size_t)best_size; // Truncate to size_t, assumes size fits
         terminal_printf("  Selected Region for Heap: Phys Addr=0x%x, Size=%u bytes (%u MB)\n",
                       best_base, (size_t)best_size, (size_t)best_size / (1024*1024));
         return true;
     } else {
         terminal_write("  Error: No suitable memory region found for heap!\n");
         return false;
     }
 }
 
 /**
  * @brief Prints a hexadecimal value with leading zeros
  * 
  * @param value Value to print
  */
 static void print_hex(uint32_t value) {
     char hex[9]; 
     hex[8] = '\0';
     
     for (int i = 0; i < 8; i++) { 
         uint8_t n = (value >> ((7 - i) * 4)) & 0xF; 
         hex[i] = (n < 10) ? ('0' + n) : ('A' + n - 10); 
     }
     
     terminal_write(hex);
 }
 
 /**
  * @brief Prints memory layout information
  * 
  * @param heap_start Physical start address of heap
  * @param heap_size Size of heap in bytes
  */
 static void print_memory_layout(uintptr_t heap_start, size_t heap_size) {
     terminal_write("\n[Kernel] Memory Layout:\n");
     terminal_write("  - Kernel Image End (Phys): 0x"); 
     print_hex(kernel_image_end_phys); 
     terminal_write("\n");
     
     terminal_write("  - Heap Start     (Phys): 0x"); 
     print_hex(heap_start); 
     terminal_write("\n");
     
     terminal_write("  - Heap Size            : "); 
     terminal_printf("%u MB\n", heap_size / (1024*1024));
     
     terminal_write("  - Heap End       (Phys): 0x"); 
     print_hex(heap_start + heap_size); 
     terminal_write("\n");
 }
 
 /**
  * @brief Initializes memory management subsystems
  * 
  * Sets up buddy allocator, paging, and kmalloc
  * 
  * @param mb_info_phys_addr Physical address of multiboot info
  * @return true on success, false on failure
  */
 static bool init_memory_management(uint32_t mb_info_phys_addr) {
     terminal_write("[Kernel] Initializing Memory Management...\n");
 
     // --- Find Memory Map ---
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(
         mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) {
         terminal_write("  [ERROR] Multiboot memory map tag not found!\n");
         return false;
     }
 
     // --- Determine Heap Region ---
     uintptr_t heap_phys_start = 0;
     size_t heap_size = 0;
     if (!find_largest_memory_area(mmap_tag, &heap_phys_start, &heap_size)) {
         terminal_write("  [ERROR] Failed to find suitable heap region from memory map.\n");
         return false;
     }
 
     // Limit heap size if needed (e.g., to fit within MAX_ORDER limit)
     size_t max_buddy_size = (size_t)1 << MAX_ORDER; // MAX_ORDER from buddy.h
     if (heap_size > max_buddy_size) {
         terminal_printf("  Warning: Largest memory region (%u MB) > Max Buddy Size (%u MB). Clamping heap size.\n",
                       heap_size / (1024*1024), max_buddy_size / (1024*1024));
         heap_size = max_buddy_size;
     }
     
     if (heap_size < MIN_HEAP_SIZE) {
         terminal_write("  [ERROR] Selected heap region is too small.\n");
         return false;
     }
 
     // Ensure heap_phys_start is page aligned
     uintptr_t aligned_heap_start = (heap_phys_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
     size_t alignment_diff = aligned_heap_start - heap_phys_start;
     if (heap_size <= alignment_diff) {
         terminal_write("  [ERROR] Heap region too small after alignment.\n");
         return false;
     }
     
     heap_phys_start = aligned_heap_start;
     heap_size -= alignment_diff;
 
     // --- Initialize Buddy Allocator ---
     terminal_printf("  Initializing Buddy Allocator (Phys Addr: 0x%x, Size: %u bytes)\n", 
                   heap_phys_start, heap_size);
     
     buddy_init((void *)heap_phys_start, heap_size);
     
     if (buddy_free_space() == 0) {
         terminal_write("  [ERROR] Buddy allocator initialization failed.\n");
         return false;
     }
     
     terminal_printf("  Buddy Allocator free space: %u bytes\n", buddy_free_space());
     print_memory_layout(heap_phys_start, heap_size);
 
     // --- Paging Setup ---
     terminal_write("  Setting up Paging...\n");
     
     // Allocate page directory using buddy
     uint32_t *initial_pd_phys = (uint32_t *)buddy_alloc(PAGE_SIZE);
     if (!initial_pd_phys) {
         terminal_write("  [ERROR] Failed to allocate kernel page directory.\n");
         return false;
     }
     
     // Map the physical address to virtual to initialize it
     uint32_t *initial_pd_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)initial_pd_phys);
     memset(initial_pd_virt, 0, PAGE_SIZE);
 
     // Calculate required mapping size
     uintptr_t required_mapping_end = heap_phys_start + heap_size;
     uint32_t phys_mapping_size = (uint32_t)required_mapping_end;
     
     // Ensure mapping is sufficiently large and properly aligned
     if (phys_mapping_size < INITIAL_PHYS_MAPPING_SIZE) {
         phys_mapping_size = INITIAL_PHYS_MAPPING_SIZE;
     }
     
     // Round up to 1MB alignment for better TLB performance
     phys_mapping_size = (phys_mapping_size + 0xFFFFF) & ~0xFFFFF;
     
     // Ensure page alignment
     phys_mapping_size = (phys_mapping_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 
     terminal_printf("  Mapping physical memory up to 0x%x (%u MB) identity & higher-half...\n", 
                   phys_mapping_size, phys_mapping_size/(1024*1024));
     
     // Create identity mapping (physical = virtual)
     if (paging_init_identity_map(initial_pd_virt, phys_mapping_size, PAGE_PRESENT | PAGE_RW) != 0) {
         terminal_write("  [ERROR] Failed identity mapping.\n");
         return false;
     }
     
     // Create higher-half mapping (physical + KERNEL_SPACE_VIRT_START = virtual)
     if (paging_map_range(initial_pd_virt, KERNEL_SPACE_VIRT_START, phys_mapping_size, 
                         PAGE_PRESENT | PAGE_RW) != 0) {
         terminal_write("  [ERROR] Failed higher-half mapping.\n");
         return false;
     }
 
     // Activate paging
     paging_set_directory(initial_pd_virt); // Store virtual address globally
     paging_activate(initial_pd_phys);      // Load physical address into CR3
     terminal_write("  [OK] Paging enabled.\n");
 
     // --- Kmalloc Initialization ---
     terminal_write("  Initializing Kmalloc Allocator...\n");
     kmalloc_init();
     terminal_write("  [OK] Kmalloc Allocator initialized.\n");
 
     terminal_write("[OK] Memory Management initialized.\n");
     return true;
 }
 
 /**
  * @brief Kernel idle task - runs when no other tasks are available
  * 
  * This is an infinite loop that halts the CPU to save power
  */
 void kernel_idle_task() {
     terminal_write("[Idle] Kernel idle task started.\n");
     
     while(1) {
         asm volatile("hlt");
     }
 }
 
 /**
  * @brief Main kernel entry point
  * 
  * @param magic Multiboot magic value
  * @param mb_info_phys_addr Physical address of multiboot info structure
  */
 void main(uint32_t magic, uint32_t mb_info_phys_addr) {
     // Initialize terminal for output
     terminal_init();
     terminal_write("=== UiAOS Kernel Booting ===\n\n");
 
     // Verify multiboot magic number
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_write("[ERROR] Invalid Multiboot magic number.\n");
         goto halt_system;
     }
     
     terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);
 
     // --- Initialize CPU Tables ---
     
     // GDT & TSS
     terminal_write("[Kernel] Initializing GDT & TSS...\n");
     gdt_init();
     terminal_write("  [OK] GDT & TSS initialized.\n");
 
     // IDT & PIC
     terminal_write("[Kernel] Initializing IDT & PIC...\n");
     idt_init();
     terminal_write("  [OK] IDT & PIC initialized.\n");
 
     // --- Memory Management ---
     if (!init_memory_management(mb_info_phys_addr)) {
         terminal_write("[FATAL] Memory management initialization failed!\n");
         goto halt_system;
     }
 
     // --- Initialize Hardware Drivers ---
     
     // Initialize Programmable Interval Timer
     terminal_write("[Kernel] Initializing PIT...\n");
     init_pit();
     terminal_write("  [OK] PIT initialized.\n");
 
     // Initialize Keyboard
     terminal_write("[Kernel] Initializing Keyboard...\n");
     keyboard_init();
     keymap_load(KEYMAP_NORWEGIAN);
     terminal_write("  [OK] Keyboard initialized.\n");
 
     // --- Initialize Filesystem ---
     terminal_write("[Kernel] Initializing Filesystem Layer...\n");
     if (fs_init() != FS_SUCCESS) {
         terminal_write("  [ERROR] Filesystem initialization failed.\n");
     } else {
         terminal_write("  [OK] Filesystem initialized.\n");
     }
 
     // --- Initialize Task Scheduler ---
     terminal_write("[Kernel] Initializing Scheduler...\n");
     scheduler_init();
     terminal_write("  [OK] Scheduler initialized.\n");
 
     // --- Create Initial User Process ---
     terminal_write("[Kernel] Creating initial user process...\n");
     const char *user_prog_path = "/hello.elf";
 
     pcb_t *user_proc_pcb = create_user_process(user_prog_path);
     if (user_proc_pcb) {
         // Mark scheduler ready before adding first task
         pit_set_scheduler_ready();
         
         if (scheduler_add_task(user_proc_pcb) == 0) {
             terminal_write("  [OK] Initial user process added to scheduler.\n");
         } else {
             terminal_write("  [ERROR] Failed to add initial process to scheduler.\n");
             destroy_process(user_proc_pcb);
         }
     } else {
         terminal_write("  [ERROR] Failed to create initial user process.\n");
     }
 
     // --- Enable Interrupts ---
     terminal_write("\n[Kernel] Enabling interrupts (STI). Starting scheduler...\n");
     __asm__ volatile ("sti");
 
     // --- Enter Idle Loop ---
     terminal_write("[Kernel] Entering main kernel idle loop (HLT).\n");
     kernel_idle_task();
 
 halt_system:
     terminal_write("\n[KERNEL HALTED]\n");
     while (1) {
         __asm__ volatile ("cli; hlt");
     }
 }