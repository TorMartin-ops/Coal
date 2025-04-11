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
 #include "buddy.h"      // *** Added include ***
 #include "frame.h"
 #include "paging.h"
 #include "kmalloc.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "elf_loader.h"
 #include "process.h"
 #include "syscall.h"
 #include "scheduler.h"
 #include "get_cpu_id.h"
 #include "fs_init.h"
 #include "read_file.h"
 #include "cpuid.h"      // *** Needs to exist (created in previous step) ***
 #include "mount.h"      // *** Added include for list_mounts ***


 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

 // Minimum heap size requirement in bytes (1MB)
 #define MIN_HEAP_SIZE (1 * 1024 * 1024)

 // Linker script symbols for kernel physical location
 extern uint32_t _kernel_start_phys; // Physical start address from linker script
 extern uint32_t _kernel_end_phys;   // Physical end address from linker script


 // --- Multiboot Tag Finding Helpers ---
 struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
     // Basic sanity check (ensure info structure itself is accessible)
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
          // Assuming accessing low memory directly is okay, but check high memory access after paging
          // A better approach would map this temporarily if needed post-paging.
          // If called before paging, direct access below 1MB is usually fine.
     }
     uint32_t total_size = *(uint32_t*)mb_info_phys_addr;
     if (total_size < 8) return NULL; // Basic sanity check

     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     uintptr_t info_end = mb_info_phys_addr + total_size;

     while (tag->type != MULTIBOOT_TAG_TYPE_END) {
         // Bounds check: Ensure current tag starts within the info structure size
         if ((uintptr_t)tag >= info_end) {
              terminal_printf("find_multiboot_tag: Error - Tag outside info bounds (Addr=0x%x, InfoEnd=0x%x)\n",
                             (uintptr_t)tag, info_end);
             return NULL;
         }
          // Check tag size is reasonable (at least size of base struct)
          if (tag->size < sizeof(struct multiboot_tag)) {
                terminal_printf("find_multiboot_tag: Error - Invalid tag size %u at Addr=0x%x\n", tag->size, (uintptr_t)tag);
               return NULL;
          }

         if (tag->type == type) return tag;

         // Advance to next tag, ensure alignment doesn't go past end
         uintptr_t next_tag_addr = (uintptr_t)tag + ((tag->size + 7) & ~7);
         // Check if next tag *starts* past the end, allowing end tag exactly at boundary
          if (next_tag_addr > info_end) {
                terminal_printf("find_multiboot_tag: Error - Next tag calculation out of bounds.\n");
                return NULL; // Avoid reading past end
          }
         tag = (struct multiboot_tag *)next_tag_addr;
     }
     return NULL; // Tag not found or reached end tag
 }

 bool find_largest_memory_area(struct multiboot_tag_mmap *mmap_tag, uintptr_t *out_base_addr, size_t *out_size) {
     uintptr_t best_base = 0; uint64_t best_size = 0;
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
     uintptr_t kernel_end_aligned = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys, PAGE_SIZE); // Align up

     terminal_write("Memory Map (from Multiboot):\n");
     while ((uintptr_t)mmap_entry < mmap_end) {
          if (mmap_tag->entry_size == 0) { // Avoid infinite loop if entry_size is bad
              terminal_write("  Error: MMAP entry size is zero!\n"); break;
          }
          // Simple print (adjust formatting if needed)
          terminal_printf("  Addr: 0x%x Len: 0x%x Type: %d\n",
                           (uint32_t)mmap_entry->addr, (uint32_t)mmap_entry->len, mmap_entry->type);

         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->addr >= 0x100000) {
             uintptr_t region_start = (uintptr_t)mmap_entry->addr;
             uint64_t region_len = mmap_entry->len;
             uintptr_t usable_start = region_start;

             // Adjust start if overlaps kernel/modules
             if (usable_start < kernel_end_aligned) {
                 if (region_start + region_len > kernel_end_aligned) {
                     uint64_t overlap = kernel_end_aligned - usable_start;
                     usable_start = kernel_end_aligned;
                     region_len = (region_len > overlap) ? region_len - overlap : 0;
                 } else { region_len = 0; } // Fully below kernel end
             }
             if (region_len > best_size) {
                 best_size = region_len; best_base = usable_start;
             }
         }
         mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
     }

     if (best_size > 0) {
         *out_base_addr = best_base;
         *out_size = (size_t)best_size; // Truncate ok? Check potential overflow if best_size > SIZE_MAX
          if (best_size > (uint64_t)(~(size_t)0)) {
              terminal_write("  Warning: Largest memory region exceeds size_t capacity!\n");
              *out_size = (~(size_t)0); // Clamp to max size_t
          }
         terminal_printf("  Selected Heap Region: Phys Addr=0x%x, Size=%u bytes (%u MB)\n",
                       best_base, *out_size, *out_size / (1024*1024));
         return true;
     }
     terminal_write("  Error: No suitable memory region found above 1MB for heap!\n");
     return false; // *** Added return false ***
 }
 // --- End Multiboot Helpers ---


 // --- Early Boot Initialization ---
 static bool init_memory(uint32_t mb_info_phys_addr) {
     terminal_write("[Kernel] Initializing Memory Subsystems...\n");

     // *** Fixed find_multiboot_tag call ***
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(
         mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) {
         terminal_write("  [FATAL] Multiboot memory map tag not found!\n");
         return false;
     }

     uintptr_t heap_phys_start = 0; size_t heap_size = 0; uintptr_t total_memory = 0;

     // Determine total memory
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
      while ((uintptr_t)mmap_entry < mmap_end) {
          if (mmap_tag->entry_size == 0) break; // Avoid loop if bad entry size
          uintptr_t region_end = (uintptr_t)mmap_entry->addr + (uintptr_t)mmap_entry->len;
          if (region_end > total_memory) total_memory = region_end;
          mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
      }
     total_memory = ALIGN_UP(total_memory, PAGE_SIZE); // Use macro from paging.h
     terminal_printf("  Detected Total Memory: %u MB\n", total_memory / (1024*1024));


     if (!find_largest_memory_area(mmap_tag, &heap_phys_start, &heap_size)) {
          terminal_write("  [FATAL] Failed to find suitable heap region.\n");
          return false;
     }
     if (heap_size < MIN_HEAP_SIZE) {
         terminal_write("  [FATAL] Heap region too small.\n");
         return false;
     }

     // Align heap start UP for buddy allocator
     // *** Use MIN_BLOCK_SIZE from buddy.h ***
     size_t required_alignment = (MIN_BLOCK_SIZE > DEFAULT_ALIGNMENT) ? MIN_BLOCK_SIZE : DEFAULT_ALIGNMENT;
     uintptr_t aligned_heap_start = ALIGN_UP(heap_phys_start, required_alignment); // Use macro
     size_t adjustment = aligned_heap_start - heap_phys_start;
     if (heap_size <= adjustment) {
         terminal_write("  [FATAL] Heap size too small after alignment.\n");
         return false;
     }
     heap_phys_start = aligned_heap_start;
     heap_size -= adjustment;

     // --- Initialize Frame Allocator FIRST ---
     terminal_write("  Initializing Frame Allocator...\n");
     if (frame_init(mmap_tag, (uintptr_t)&_kernel_start_phys, (uintptr_t)&_kernel_end_phys, heap_phys_start, heap_phys_start + heap_size) != 0) {
         terminal_write("  [FATAL] Frame Allocator initialization failed!\n");
         return false;
     }

     // --- Initialize Buddy Allocator ---
     terminal_write("  Initializing Buddy Allocator...\n");
     buddy_init((void *)heap_phys_start, heap_size);
     // *** Use MIN_BLOCK_SIZE from buddy.h ***
     if (buddy_free_space() == 0 && heap_size >= MIN_BLOCK_SIZE) {
         terminal_write("  [FATAL] Buddy Allocator initialization failed!\n");
         return false;
     }
     terminal_printf("  Buddy Free Space: %u bytes\n", buddy_free_space());


     // --- Initialize Paging ---
     terminal_write("  Initializing Paging...\n");
     if (paging_init((uintptr_t)&_kernel_start_phys, (uintptr_t)&_kernel_end_phys, total_memory) != 0) {
          terminal_write("  [FATAL] Paging initialization failed!\n");
          return false;
     }

     // --- Initialize Kmalloc ---
     terminal_write("  Initializing Kmalloc...\n");
     kmalloc_init();

     terminal_write("[OK] Memory Subsystems Initialized.\n");
     return true;
 }


 // Kernel idle task
 void kernel_idle_task() {
     terminal_write("[Idle] Kernel idle task started. Halting CPU.\n");
     while(1) { asm volatile("sti; hlt"); } // Enable interrupts before halting
 }

 // Main kernel entry point
 void main(uint32_t magic, uint32_t mb_info_phys_addr) {
     terminal_init();
     terminal_write("=== UiAOS Kernel Booting ===\n\n");

     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
         terminal_write("[FATAL] Invalid Multiboot magic number.\n");
         goto halt_system;
     }
     terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);

     // --- Core Subsystems Init ---
     terminal_write("[Kernel] Initializing GDT & TSS...\n"); gdt_init();
     terminal_write("[Kernel] Initializing IDT & PIC...\n"); idt_init();

     // --- Memory Management (Calls Frame, Buddy, Paging, Kmalloc in order) ---
     if (!init_memory(mb_info_phys_addr)) {
         goto halt_system;
     }

     // --- Hardware Drivers (Initialize after memory is ready) ---
     terminal_write("[Kernel] Initializing PIT...\n"); init_pit();
     terminal_write("[Kernel] Initializing Keyboard...\n"); keyboard_init(); keymap_load(KEYMAP_NORWEGIAN);

     // --- Filesystem ---
     terminal_write("[Kernel] Initializing Filesystem...\n");
     if (fs_init() == FS_SUCCESS) {
         // *** Use list_mounts (needs mount.h) ***
         list_mounts();
     } else {
         terminal_write("  [Warning] Filesystem initialization failed.\n");
     }

     // --- Scheduler & Initial Process ---
     terminal_write("[Kernel] Initializing Scheduler...\n"); scheduler_init();
     terminal_write("[Kernel] Creating initial user process...\n");
     const char *user_prog_path = "/hello.elf";
     pcb_t *user_proc_pcb = create_user_process(user_prog_path);
     if (user_proc_pcb) {
         pit_set_scheduler_ready();
         if (scheduler_add_task(user_proc_pcb) != 0) {
              terminal_write("  [ERROR] Failed to add initial process to scheduler.\n");
             destroy_process(user_proc_pcb);
         } else {
             terminal_write("  [OK] Initial user process added to scheduler.\n");
         }
     } else {
         terminal_printf("  [ERROR] Failed to create initial user process from '%s'.\n", user_prog_path);
     }

     // --- Enable Interrupts and Start ---
     terminal_write("\n[Kernel] Enabling interrupts & starting idle task...\n");
     kernel_idle_task(); // Enters hlt loop after enabling interrupts in idle task

 halt_system:
     terminal_write("\n[KERNEL HALTED]\n");
     while (1) { __asm__ volatile ("cli; hlt"); }
 }