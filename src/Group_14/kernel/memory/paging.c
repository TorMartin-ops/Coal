/**
 * paging.c - Paging Implementation (32-bit x86 with PSE and NX via EFER)
 */

 #include <kernel/memory/paging.h>
 #include <kernel/memory/frame.h>              // Frame allocator (get_frame, put_frame, frame_alloc)
 #include <kernel/memory/buddy.h>              // Buddy allocator (BUDDY_ALLOC, BUDDY_FREE)
 #include <kernel/drivers/display/terminal.h>           // Kernel console output
 #include <kernel/core/types.h>              // Core type definitions
 #include <kernel/process/process.h>            // For page fault context
 #include <kernel/memory/mm.h>                 // For VMA handling during page faults
 #include <kernel/process/scheduler.h>          // For terminating process on critical fault
 #include <kernel/lib/string.h>             // Kernel's memset, memcpy
 #include <kernel/cpu/cpuid.h>              // CPUID instruction wrapper
 #include <kernel/memory/kmalloc_internal.h>   // For ALIGN_UP/DOWN macros
 #include <kernel/arch/multiboot2.h>
 #include <kernel/cpu/msr.h>                // For MSR read/write (EFER)
 #include <kernel/lib/assert.h>             // For KERNEL_ASSERT
#include <kernel/drivers/display/serial.h>             // Serial port logging

 // --- Constants and Macros ---
 #ifndef PAGING_PANIC
 #define PAGING_PANIC(msg) do { \
         serial_printf("\n[PAGING PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
         while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 #endif

 #ifndef ARRAY_SIZE
 #define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
 #endif

 #define MIN(a, b) (((a) < (b)) ? (a) : (b))

 // Define a dedicated virtual address for temporary mappings.
 // Ensure this address doesn't conflict with kernel, heap, stacks, or MMIO.
 // Often placed just below the recursive mapping area.
 // IMPORTANT: This PTE slot MUST exist and be reserved for this purpose (ensure PDE[1022] exists).
 // #define PAGING_TEMP_VADDR 0xFFBFF000 // Example: Last page in the PT mapped by PDE 1022
 // Note: PAGING_TEMP_VADDR is now managed dynamically by the temp map allocator.

 // --- Debugging Control ---
 #define PAGING_DEBUG 1 // Set to 0 to disable debug prints

 // --- ADDED DEBUG MACRO DEFINITION ---
 #if PAGING_DEBUG
 #define PAGING_DEBUG_PRINTF(fmt, ...) \
     do { \
         serial_printf("[Paging DEBUG %s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__); \
     } while (0)
 #else
 #define PAGING_DEBUG_PRINTF(fmt, ...) do {} while (0) // Define as empty block when disabled
 #endif
 // --- END DEBUG MACRO DEFINITION ---

 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL;
 uint32_t  g_kernel_page_directory_phys = 0;
 bool      g_pse_supported              = false;
 bool      g_nx_supported               = false;

 // --- Linker Symbols ---
 extern uint8_t _kernel_start_phys;
 extern uint8_t _kernel_end_phys;
 extern uint8_t _kernel_text_start_phys;
 extern uint8_t _kernel_text_end_phys;
 extern uint8_t _kernel_rodata_start_phys;
 extern uint8_t _kernel_rodata_end_phys;
 extern uint8_t _kernel_data_start_phys;
 extern uint8_t _kernel_data_end_phys;


 #define KERN_EINVAL -1 // Invalid argument (e.g., alignment)
 #define KERN_ENOENT -2 // Entry not found / Not mapped
 #define KERN_EPERM  -3 // Operation not permitted (e.g., on 4MB page)
 #define KERN_ENOMEM -4 // Out of memory (Resource allocation failure)
 #define KERN_EEXIST -5 // Resource already exists (Mapping conflict)

 // --- Multiboot Info ---
 extern uint32_t g_multiboot_info_phys_addr_global;

 // --- Early Allocation Tracking ---
 #define MAX_EARLY_ALLOCATIONS 256
 static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
 static int       early_allocated_count  = 0;
 static bool      early_allocator_used   = false; // Tracks if early allocator is ACTIVE, becomes false after finalize

 // --- External Assembly Functions ---
 extern void paging_invalidate_page(void *vaddr);
 extern void paging_activate(uint32_t *page_directory_phys);

 // --- Forward Declarations (Internal Functions) ---
 static int        map_page_internal(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static bool       check_and_enable_nx(void);
 static uintptr_t  paging_alloc_frame(bool use_early_allocator);
 static uint32_t* allocate_page_table_phys(bool use_early_allocator);
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type);
 static int        kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
 static int        paging_map_physical_early(uintptr_t page_directory_phys, uintptr_t phys_addr_start, size_t size, uint32_t flags, bool map_to_higher_half);
 static void       debug_print_pd_entries(uint32_t* pd_ptr, uintptr_t vaddr_start, size_t count); // Changed first arg type
 static bool is_page_table_empty(uint32_t *pt_virt);


 // --- Low-Level CPU Control ---
 static inline uint32_t read_cr4(void) {
         uint32_t v;
         asm volatile("mov %%cr4, %0" : "=r"(v));
         return v;
 }

 static inline void write_cr4(uint32_t v) {
         asm volatile("mov %0, %%cr4" :: "r"(v) : "memory");  // "memory" clobber
 }

 static inline void enable_cr4_pse(void) {
         write_cr4(read_cr4() | (1 << 4));
 }

 // --- Low-Level Temporary Mappings (Use with EXTREME caution) ---
 // Marked static as it's an internal unsafe helper
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
     PAGING_DEBUG_PRINTF("Enter: vaddr=%p, paddr=%#lx, flags=%#lx\n", (void*)vaddr, (unsigned long)paddr, flags);

     if (!g_kernel_page_directory_virt) {
         serial_printf("[KMapUnsafe] Warning: Kernel PD Virt not set (may be okay pre-activation)\n");
         if (!g_kernel_page_directory_virt) return -1; // Hard fail if called post-activation without valid virt ptr
     }

     // Check alignment and range
     if ((vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) {
        terminal_printf("[KMapUnsafe] Error: Unaligned addresses V=%#lx P=%#lx\n", (unsigned long)vaddr, (unsigned long)paddr);
         return -1;
     }

     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     PAGING_DEBUG_PRINTF("Calculated: pd_idx=%lu, pt_idx=%lu\n", (unsigned long)pd_idx, (unsigned long)pt_idx);


     // Access kernel PD using its virtual address (ASSUMES PAGING ACTIVE or special setup)
     PAGING_DEBUG_PRINTF("Accessing kernel PD virt %p at index %lu\n", g_kernel_page_directory_virt, (unsigned long)pd_idx);
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
     PAGING_DEBUG_PRINTF("Read PDE value: %#lx\n", (unsigned long)pde);

     // If PDE not present, we need to create a new page table
     if (!(pde & PAGE_PRESENT)) {
         PAGING_DEBUG_PRINTF("PDE not present. Allocating new PT...\n");
         // Allocate a new page table using frame allocator (NOT early allocator here, assumes buddy is up)
         uintptr_t new_pt_phys = frame_alloc();
         if (new_pt_phys == 0) {
             terminal_printf("[KMapUnsafe] Error: Failed to allocate PT for VAddr %p\n", (void*)vaddr);
             return -1;
         }
         PAGING_DEBUG_PRINTF("Allocated new PT frame at P=%#lx\n", (unsigned long)new_pt_phys);

         // Map the PT in the page directory
         // Ensure flags don't include user bit unless explicitly intended
         uint32_t pde_flags_for_pt = PAGE_PRESENT | PAGE_RW; // Kernel RW default for new PT
         // Inherit User flag if the final mapping needs it
         if (flags & PAGE_USER) {
             pde_flags_for_pt |= PAGE_USER;
         }
         uint32_t new_pde_val = (new_pt_phys & PAGING_ADDR_MASK) | pde_flags_for_pt;
         PAGING_DEBUG_PRINTF("Setting PDE[%lu] = %#lx\n", (unsigned long)pd_idx, (unsigned long)new_pde_val);
         g_kernel_page_directory_virt[pd_idx] = new_pde_val;
         paging_invalidate_page((void*)vaddr); // Invalidate old mapping potentially covering this vaddr range

         // Now we need to clear the new PT - use recursive mapping to access it
         uint32_t* new_pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
         PAGING_DEBUG_PRINTF("Clearing new PT via recursive mapping at %p...\n", new_pt_virt);

         // Clear the PT
         memset(new_pt_virt, 0, PAGE_SIZE);

         // Now set the specific PTE entry
         uint32_t new_pte_val = (paddr & PAGING_ADDR_MASK) | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
         PAGING_DEBUG_PRINTF("Setting PTE[%lu] in new PT = %#lx\n", (unsigned long)pt_idx, (unsigned long)new_pte_val);
         new_pt_virt[pt_idx] = new_pte_val;
         paging_invalidate_page((void*)vaddr); // Invalidate the specific vaddr
         PAGING_DEBUG_PRINTF("Exit OK (new PT path)\n");
         return 0;
     }

     // Check if this is a 4MB page
     if (pde & PAGE_SIZE_4MB) {
         serial_printf("[KMapUnsafe] Error: Cannot map 4KB page into existing 4MB PDE V=%p\n", (void*)vaddr);
         return -1;
     }

     // PDE exists and points to a 4KB page table
     // Use recursive mapping to access the existing PT
     uintptr_t pt_phys_addr = pde & PAGING_ADDR_MASK; // Get PT phys addr for debug
     uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
     PAGING_DEBUG_PRINTF("PDE exists, accessing existing PT via recursive mapping %p (Phys %#lx)\n", pt_virt, (unsigned long)pt_phys_addr);

     // Check if PTE already exists
     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         uintptr_t existing_paddr = pt_virt[pt_idx] & PAGING_ADDR_MASK;
         if (existing_paddr != (paddr & PAGING_ADDR_MASK)) {
             terminal_printf("[KMapUnsafe] Warning: Overwriting existing PTE for V=%p (Old P=%#lx, New P=%#lx)\n",
                             (void*)vaddr, (unsigned long)existing_paddr, (unsigned long)paddr);
         }
         // Allow overwriting identical mapping or changing flags
     }

     // Set the PTE entry
     uint32_t new_pte_val = (paddr & PAGING_ADDR_MASK) | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
     PAGING_DEBUG_PRINTF("Setting PTE[%lu] in existing PT = %#lx\n", (unsigned long)pt_idx, (unsigned long)new_pte_val);
     pt_virt[pt_idx] = new_pte_val;
     paging_invalidate_page((void*)vaddr);
     PAGING_DEBUG_PRINTF("Exit OK (existing PT path)\n");
     return 0;
 }


 // --- Early Memory Allocation ---
 // Marked static as it's an internal early boot helper
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
      // Access MB info directly via physical addr (ASSUMES <1MB or identity mapped)
      // Need volatile as memory contents can change unexpectedly before caching/MMU setup.
      if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Basic sanity check for low memory
          serial_printf("[MB Early] Invalid MB info address %#lx\n", (unsigned long)mb_info_phys_addr);
          return NULL;
      }
      volatile uint32_t* mb_info_ptr = (volatile uint32_t*)mb_info_phys_addr;
      uint32_t total_size = mb_info_ptr[0]; // First field is total size

      // Sanity check size
      if (total_size < 8 || total_size > 16 * 1024) { // Header is 8 bytes, max reasonable size e.g. 16KB
          terminal_printf("[MB Early] Invalid MB total size %lu\n", (unsigned long)total_size);
          return NULL;
      }

      struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8); // Tags start after size and reserved fields
      uintptr_t info_end        = mb_info_phys_addr + total_size;

      // Iterate through tags
      while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
          uintptr_t current_tag_addr = (uintptr_t)tag;
          // Check tag bounds
          if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || // Ensure basic tag header fits
              tag->size < 8 ||                                            // Minimum tag size
              current_tag_addr + tag->size > info_end) {                   // Ensure full tag fits within total_size
              terminal_printf("[MB Early] Invalid tag found at %#lx (type %u, size %lu)\n", (unsigned long)current_tag_addr, (unsigned int)tag->type, (unsigned long)tag->size);
              return NULL; // Invalid tag structure
          }

          if (tag->type == type) {
              return tag; // Found the tag
          }

          // Move to the next tag (align size to 8 bytes)
          uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
          if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) { // Check for loop or overflow
              terminal_printf("[MB Early] Invalid next tag address %#lx calculated from tag at %#lx\n", (unsigned long)next_tag_addr, (unsigned long)current_tag_addr);
              break;
          }
          tag = (struct multiboot_tag *)next_tag_addr;
      }

      return NULL; // Tag not found
 }

 // Marked static as it's an internal early boot helper
 static uintptr_t paging_alloc_early_frame_physical(void) {
      early_allocator_used = true; // Mark that we are using it now
      if (g_multiboot_info_phys_addr_global == 0) {
          terminal_write("[EARLY ALLOC ERROR] Multiboot info address is zero!\n");
          PAGING_PANIC("Early alloc attempted before Multiboot info set!");
      }

      struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)
          find_multiboot_tag_early(g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);

      if (!mmap_tag) {
          PAGING_PANIC("Early alloc failed: Multiboot MMAP tag not found!");
      }

      // Check mmap tag structure basics
      if (mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
           PAGING_PANIC("Early alloc failed: MMAP entry size too small!");
      }

      uintptr_t kernel_start_p = (uintptr_t)&_kernel_start_phys;
      uintptr_t kernel_end_p   = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys);
      // Also avoid the multiboot info structure itself
      uint32_t mb_info_size = *(volatile uint32_t*)g_multiboot_info_phys_addr_global;
      mb_info_size = (mb_info_size >= 8) ? mb_info_size : 8; // Minimum size
      uintptr_t mb_info_start = g_multiboot_info_phys_addr_global;
      uintptr_t mb_info_end = PAGE_ALIGN_UP(mb_info_start + mb_info_size);


      multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
      uintptr_t mmap_tag_end = (uintptr_t)mmap_tag + mmap_tag->size;

      // Iterate through memory map entries
      while ((uintptr_t)mmap_entry < mmap_tag_end) {
          // Ensure the current entry structure itself is within the tag boundaries
          if ((uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_tag_end) {
              PAGING_PANIC("Invalid MMAP tag structure: entry exceeds tag boundary");
          }

          if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
              uintptr_t region_start = (uintptr_t)mmap_entry->addr;
              uintptr_t region_len   = (uintptr_t)mmap_entry->len;
              uintptr_t region_end   = region_start + region_len;

              // Check for overflow on region_end calculation
              if (region_end < region_start) {
                  region_end = UINTPTR_MAX;
              }

              // Align start address UP to the nearest page boundary within the region
              uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);

              // Iterate through potential frames in the current available region
              while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr) {
                  // Check if the frame itself fits within the region
                  if (current_frame_addr + PAGE_SIZE > region_end) {
                      break; // This frame would extend beyond the available region
                  }

                  // Skip frames below 1MB (often reserved or problematic)
                  if (current_frame_addr < 0x100000) {
                      current_frame_addr += PAGE_SIZE;
                      continue;
                  }

                  // Check overlap with kernel physical memory
                  bool overlaps_kernel = (current_frame_addr < kernel_end_p &&
                                        (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                  if (overlaps_kernel) {
                      current_frame_addr += PAGE_SIZE;
                      continue;
                  }

                  // Check overlap with multiboot info structure
                   bool overlaps_mb_info = (current_frame_addr < mb_info_end &&
                                          (current_frame_addr + PAGE_SIZE) > mb_info_start);
                   if (overlaps_mb_info) {
                       current_frame_addr += PAGE_SIZE;
                       continue;
                   }


                  // Check if already allocated by this early allocator
                  bool already_allocated = false;
                  for (int i = 0; i < early_allocated_count; ++i) {
                      if (early_allocated_frames[i] == current_frame_addr) {
                          already_allocated = true;
                          break;
                      }
                  }
                  if (already_allocated) {
                      current_frame_addr += PAGE_SIZE;
                      continue;
                  }

                  // Found a suitable frame!
                  if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) {
                      PAGING_PANIC("Exceeded MAX_EARLY_ALLOCATIONS!");
                  }

                  // Record and return the frame
                  early_allocated_frames[early_allocated_count++] = current_frame_addr;

                  // Zero the frame before returning - critical for page tables!
                  memset((void*)current_frame_addr, 0, PAGE_SIZE);

                  return current_frame_addr;
              }
          }

          // Move to the next entry
          uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
          // Check for loop/overflow/invalid entry size
          if (next_entry_addr <= (uintptr_t)mmap_entry || next_entry_addr > mmap_tag_end) {
              PAGING_PANIC("Invalid MMAP iteration");
              break;
          }
          mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
      }

      PAGING_PANIC("Early alloc failed: No suitable physical frame found!");
      return 0; // Unreachable
 }

 // --- Unified Frame/Page Table Allocation ---
 // Marked static as it's an internal helper
 static uintptr_t paging_alloc_frame(bool use_early_allocator) {
      if (use_early_allocator) {
          if (early_allocator_used) { // Check if early allocator is still the active one
              return paging_alloc_early_frame_physical();
          } else {
              PAGING_PANIC("Attempted early frame allocation after early stage finished!");
          }
      } else {
          // Use normal frame allocator
          uintptr_t frame = frame_alloc();
          if (frame == 0) {
              PAGING_PANIC("frame_alloc() failed during normal allocation!");
          }
          // Assuming frame_alloc returns zeroed frames
          return frame;
      }
      return 0; // Should be unreachable
 }

 // Marked static as it's an internal helper
 static uint32_t* allocate_page_table_phys(bool use_early_allocator) {
      uintptr_t pt_phys = paging_alloc_frame(use_early_allocator);
      if (!pt_phys) {
          serial_printf("[Paging] Failed to allocate frame for Page Table (early=%d).\n",
                          use_early_allocator);
          return NULL;
      }
      // Frame is already zeroed
      return (uint32_t*)pt_phys;
 }

 // --- CPU Feature Detection ---
 bool check_and_enable_pse(void) {
      uint32_t eax, ebx, ecx, edx;
      cpuid(1, &eax, &ebx, &ecx, &edx); // Basic CPUID info

      if (edx & CPUID_FEAT_EDX_PSE) {
          terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
          enable_cr4_pse();
          if (read_cr4() & CR4_PSE) {
              terminal_write("[Paging] CR4.PSE bit enabled.\n");
              g_pse_supported = true;
              return true;
          } else {
              terminal_write("[Paging Error] Failed to enable CR4.PSE bit!\n");
              g_pse_supported = false;
              return false;
          }
      } else {
          terminal_write("[Paging] CPU does not support PSE (4MB Pages).\n");
          g_pse_supported = false;
          return false;
      }
 }

 static bool check_and_enable_nx(void) {
      uint32_t eax, ebx, ecx, edx;
      cpuid(0x80000000, &eax, &ebx, &ecx, &edx); // Get highest extended function supported

      if (eax < 0x80000001) {
          terminal_write("[Paging] CPUID extended function 0x80000001 not supported. Cannot check NX.\n");
          g_nx_supported = false;
          return false;
      }

      cpuid(0x80000001, &eax, &ebx, &ecx, &edx); // Get extended feature bits

      if (edx & CPUID_FEAT_EDX_NX) {
          terminal_write("[Paging] CPU supports NX (Execute Disable) bit.\n");
          uint64_t efer = rdmsr(MSR_EFER);
          efer |= EFER_NXE; // Set bit 11
          wrmsr(MSR_EFER, efer);

          efer = rdmsr(MSR_EFER);
          if (efer & EFER_NXE) {
              terminal_write("[Paging] EFER.NXE bit enabled.\n");
              g_nx_supported = true;
              return true;
          } else {
              terminal_write("[Paging Error] Failed to enable EFER.NXE bit!\n");
              g_nx_supported = false;
              return false;
          }
      } else {
          terminal_write("[Paging] CPU does not support NX bit.\n");
          g_nx_supported = false;
          return false;
      }
 }


 // --- Paging Initialization Stages ---

 int paging_initialize_directory(uintptr_t* out_initial_pd_phys) {
      terminal_write("[Paging Stage 1] Initializing Page Directory...\n");
      uintptr_t pd_phys = paging_alloc_early_frame_physical();
      if (!pd_phys) {
          PAGING_PANIC("Failed to allocate initial Page Directory frame!");
      }
      terminal_printf("  Allocated initial PD at Phys: %#lx\n", (unsigned long)pd_phys);

      if (!check_and_enable_pse()) {
          PAGING_PANIC("PSE support is required but not available/enabled!");
      }
      check_and_enable_nx();

      *out_initial_pd_phys = pd_phys;
      terminal_write("[Paging Stage 1] Directory allocated, features checked/enabled.\n");
      return 0; // Success
 }


 /**
  * @brief Maps a physical memory range to virtual addresses before paging is active.
  * Uses the early frame allocator for page tables. Directly manipulates physical PD/PT.
  */
 static int paging_map_physical_early(uintptr_t page_directory_phys,
                                      uintptr_t phys_addr_start,
                                      size_t size,
                                      uint32_t flags,
                                      bool map_to_higher_half)
 {
      if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0 || size == 0) {
          serial_printf("[Paging Early Map] Invalid PD phys (%#lx) or size (%lu).\n",
                          (unsigned long)page_directory_phys, (unsigned long)size);
          return -1;
      }

      uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_start);
      uintptr_t end_phys;
      if (phys_addr_start > UINTPTR_MAX - size) {
           end_phys = UINTPTR_MAX;
      } else {
          end_phys = phys_addr_start + size;
      }
      uintptr_t aligned_end_phys = PAGE_ALIGN_UP(end_phys);
      if (aligned_end_phys < end_phys) {
           aligned_end_phys = UINTPTR_MAX;
      }
      end_phys = aligned_end_phys;

      if (end_phys <= current_phys) {
          return 0; // Size is zero or negative after alignment
      }

      size_t map_size = (current_phys < end_phys) ? (end_phys - current_phys) : 0;
      volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;

      terminal_printf("  Mapping Phys [%#lx - %#lx) -> %s (Size: %lu KB) with flags %#lx\n",
                      (unsigned long)current_phys, (unsigned long)end_phys,
                      map_to_higher_half ? "HigherHalf" : "Identity",
                      (unsigned long)(map_size / 1024),
                      (unsigned long)flags);

      int page_count = 0;
      int safety_counter = 0;
      const int max_pages_early = (1024 * 1024 * 1024) / PAGE_SIZE;

      while (current_phys < end_phys) {
          if (++safety_counter > max_pages_early) {
              serial_printf("[Paging Early Map] Warning: Safety break after %d pages\n", safety_counter);
              return -1;
          }

          uintptr_t target_vaddr;
          if (map_to_higher_half) {
              if (current_phys > UINTPTR_MAX - KERNEL_SPACE_VIRT_START) {
                  serial_printf("[Paging Early Map] Virtual address overflow for Phys %#lx to Higher Half\n", (unsigned long)current_phys);
                  return -1;
              }
              target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
          } else {
              target_vaddr = current_phys;
              if (target_vaddr >= KERNEL_SPACE_VIRT_START) {
                  serial_printf("[Paging Early Map] Warning: Identity map target %#lx overlaps kernel space start %#lx\n",
                                  (unsigned long)target_vaddr, (unsigned long)KERNEL_SPACE_VIRT_START);
              }
          }

          if (target_vaddr % PAGE_SIZE != 0) {
              serial_printf("[Paging Early Map] Internal Error: Target VAddr %#lx not aligned.\n", (unsigned long)target_vaddr);
              return -1;
          }

          uint32_t pd_idx = PDE_INDEX(target_vaddr);
          uint32_t pt_idx = PTE_INDEX(target_vaddr);

          uint32_t pde = pd_phys_ptr[pd_idx];
          uintptr_t pt_phys_addr;
          volatile uint32_t* pt_phys_ptr;

          if (!(pde & PAGE_PRESENT)) {
              uint32_t* new_pt = allocate_page_table_phys(true);
              if (!new_pt) {
                  serial_printf("[Paging Early Map] Failed to allocate PT frame for VAddr %#lx\n", (unsigned long)target_vaddr);
                  return -1;
              }
              pt_phys_addr = (uintptr_t)new_pt;
              pt_phys_ptr = (volatile uint32_t*)pt_phys_addr;

              uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
              if (flags & PAGE_USER) { pde_flags |= PAGE_USER; }
              pd_phys_ptr[pd_idx] = (pt_phys_addr & PAGING_ADDR_MASK) | pde_flags;
          } else {
              if (pde & PAGE_SIZE_4MB) {
                  serial_printf("[Paging Early Map] Error: Attempt to map 4K page over existing 4M page at VAddr %#lx (PDE[%u]=%#lx)\n",
                                  (unsigned long)target_vaddr, (unsigned int)pd_idx, (unsigned long)pde);
                  return -1;
              }
              pt_phys_addr = (uintptr_t)(pde & PAGING_ADDR_MASK);
              pt_phys_ptr = (volatile uint32_t*)pt_phys_addr;

              uint32_t needed_pde_flags = PAGE_PRESENT;
              if (flags & PAGE_RW) needed_pde_flags |= PAGE_RW;
              if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;

              if ((pde & needed_pde_flags) != needed_pde_flags) {
                   pd_phys_ptr[pd_idx] |= (needed_pde_flags & (PAGE_RW | PAGE_USER));
              }
          }

          uint32_t pte = pt_phys_ptr[pt_idx];
          uint32_t pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD)) | PAGE_PRESENT;
          uint32_t new_pte = (current_phys & PAGING_ADDR_MASK) | pte_final_flags;

          if (pte & PAGE_PRESENT) {
              if (pte != new_pte) {
                  serial_printf("[Paging Early Map] Error: PTE already present/different for VAddr %#lx (PTE[%lu])\n", (unsigned long)target_vaddr, (unsigned long)pt_idx);
                  serial_printf("  Existing PTE = %#lx (Points to Phys %#lx)\n", (unsigned long)pte, (unsigned long)(pte & PAGING_ADDR_MASK));
                  terminal_printf("  Attempted PTE = %#lx (Points to Phys %#lx)\n", (unsigned long)new_pte, (unsigned long)(new_pte & PAGING_ADDR_MASK));
                  return -1;
              }
          }

          pt_phys_ptr[pt_idx] = new_pte;
          page_count++;

          if (current_phys > UINTPTR_MAX - PAGE_SIZE) { break; }
          current_phys += PAGE_SIZE;
      }

      terminal_printf("  Mapped %d pages for region.\n", page_count);
      return 0; // Success
 }

int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start,
                             uintptr_t kernel_phys_end,
                             uintptr_t heap_phys_start,
                             size_t heap_size)
 {
      terminal_write("[Paging Stage 2] Setting up early memory maps...\n");
      volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys; // Make sure pd_phys_ptr is declared early

      // --- Existing Mappings ---
      size_t identity_map_size = 4 * 1024 * 1024; // 4MB
      terminal_printf("  Mapping Identity [0x0 - 0x%zx)\n", identity_map_size);
      if (paging_map_physical_early(page_directory_phys, 0x0, identity_map_size, PTE_KERNEL_DATA_FLAGS, false) != 0) {
          PAGING_PANIC("Failed to set up early identity mapping!");
      }

      uintptr_t kernel_phys_aligned_start = PAGE_ALIGN_DOWN(kernel_phys_start);
      uintptr_t kernel_phys_aligned_end = PAGE_ALIGN_UP(kernel_phys_end);
      size_t kernel_size = kernel_phys_aligned_end - kernel_phys_aligned_start;
      terminal_printf("  Mapping Kernel Phys [%#lx - %#lx) to Higher Half [%#lx - %#lx)\n",
        (unsigned long)kernel_phys_aligned_start, (unsigned long)kernel_phys_aligned_end,
        (unsigned long)(KERNEL_SPACE_VIRT_START + kernel_phys_aligned_start),
        (unsigned long)(KERNEL_SPACE_VIRT_START + kernel_phys_aligned_end));
      if (paging_map_physical_early(page_directory_phys, kernel_phys_aligned_start, kernel_size, PTE_KERNEL_DATA_FLAGS, true) != 0) {
          PAGING_PANIC("Failed to map kernel to higher half!");
      }

      if (heap_size > 0) {
          uintptr_t heap_phys_aligned_start = PAGE_ALIGN_DOWN(heap_phys_start);
          uintptr_t heap_end = heap_phys_start + heap_size;
          uintptr_t heap_phys_aligned_end = PAGE_ALIGN_UP(heap_end);
          if(heap_phys_aligned_end < heap_end) heap_phys_aligned_end = UINTPTR_MAX;
          size_t heap_aligned_size = heap_phys_aligned_end - heap_phys_aligned_start;
          terminal_printf("  Mapping Kernel Heap Phys [%#lx - %#lx) to Higher Half [%#lx - %#lx)\n",
                (unsigned long)heap_phys_aligned_start, (unsigned long)heap_phys_aligned_end,
                (unsigned long)(KERNEL_SPACE_VIRT_START + heap_phys_aligned_start),
                (unsigned long)(KERNEL_SPACE_VIRT_START + heap_phys_aligned_end));
          if (paging_map_physical_early(page_directory_phys, heap_phys_aligned_start, heap_aligned_size, PTE_KERNEL_DATA_FLAGS, true) != 0) {
              PAGING_PANIC("Failed to map early kernel heap!");
          }
      }

      terminal_printf("  Mapping VGA Buffer Phys %#lx to Virt %#lx\n", (unsigned long)VGA_PHYS_ADDR, (unsigned long)VGA_VIRT_ADDR);
      if (paging_map_physical_early(page_directory_phys, VGA_PHYS_ADDR, PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true) != 0) {
          PAGING_PANIC("Failed to map VGA buffer!");
      }

      // --- NEW: Pre-allocate Kernel Stack Page Tables ---
      terminal_printf("  Pre-allocating Page Tables for Kernel Stack Range [0x%lx - 0x%lx)...\n",
                      (unsigned long)KERNEL_STACK_VADDR_START, (unsigned long)KERNEL_TEMP_MAP_START);

      uint32_t stack_start_pde_idx = PDE_INDEX(KERNEL_STACK_VADDR_START); // e.g., 896 for 0xE0000000
      uint32_t stack_end_pde_idx   = PDE_INDEX(KERNEL_TEMP_MAP_START);    // e.g., 1016 for 0xFE000000

      for (uint32_t pde_idx = stack_start_pde_idx; pde_idx < stack_end_pde_idx; ++pde_idx) {
          if (!(pd_phys_ptr[pde_idx] & PAGE_PRESENT)) {
              uintptr_t new_pt_phys = paging_alloc_early_frame_physical();
              if (!new_pt_phys) {
                  KERNEL_PANIC_HALT("Failed to allocate PT frame for kernel stack!");
              }

              // Set PDE flags: Present, RW, Kernel, NX (if supported)
              uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
              if (g_nx_supported) {
                  pde_flags |= PAGE_NX_BIT;
              }
              // Set the PDE entry in the global kernel directory
              pd_phys_ptr[pde_idx] = (new_pt_phys & PAGING_ADDR_MASK) | pde_flags;
              // Debug print (optional)
              // terminal_printf("    Allocated Kernel Stack PT: PDE[%lu] -> Phys %#lx\n", (unsigned long)pde_idx, (unsigned long)new_pt_phys);
          }
      }
      serial_printf("   Kernel Stack PTs pre-allocation check complete.\n");
      // --- End NEW Section ---


      // --- Existing: Pre-allocate Temp Map Area PT ---
      const uint32_t temp_map_pde_index = PDE_INDEX(KERNEL_TEMP_MAP_START); // Should evaluate to 1016
      terminal_printf("  Pre-allocating Page Table for Temporary Mapping Area (PDE %lu)...\n", (unsigned long)temp_map_pde_index);
      if (!(pd_phys_ptr[temp_map_pde_index] & PAGE_PRESENT)) { // Check if it wasn't already allocated by the stack loop (unlikely but safe)
          uintptr_t temp_pt_phys = paging_alloc_early_frame_physical();
          if (!temp_pt_phys) {
              KERNEL_PANIC_HALT("Failed to allocate PT frame for temporary mapping area!");
          }
          uint32_t temp_pde_flags = PAGE_PRESENT | PAGE_RW;
          if (g_nx_supported) {
              temp_pde_flags |= PAGE_NX_BIT;
          }
          pd_phys_ptr[temp_map_pde_index] = (temp_pt_phys & PAGING_ADDR_MASK) | temp_pde_flags;
          terminal_printf("   Mapped PDE[%lu] to PT Phys %#lx\n", (unsigned long)temp_map_pde_index, (unsigned long)temp_pt_phys);
      } else {
           serial_printf("   PT for Temporary Mapping Area (PDE %lu) already exists.\n", (unsigned long)temp_map_pde_index);
      }


      terminal_write("[Paging Stage 2] Early memory maps configured.\n");
      return 0; // Success
 }

 static void debug_print_pd_entries(uint32_t* pd_ptr, uintptr_t vaddr_start, size_t count) {
      terminal_write("--- Debug PD Entries ---\n");
      uint32_t start_idx = PDE_INDEX(vaddr_start);
      uint32_t end_idx = start_idx + count;
      if (end_idx > TABLES_PER_DIR) end_idx = TABLES_PER_DIR;

      for (uint32_t idx = start_idx; idx < end_idx; ++idx) {
          uint32_t pde = pd_ptr[idx];
          uintptr_t va = (uintptr_t)idx << PAGING_PDE_SHIFT;

          if (pde & PAGE_PRESENT) {
              terminal_printf(" PDE[%4u] (V~%#08lx): %#08lx (P=%d RW=%d US=%d PS=%d",
                (unsigned int)idx,
                (unsigned long)va,
                (unsigned long)pde,
                              (pde & PAGE_PRESENT) ? 1 : 0,
                              (pde & PAGE_RW) ? 1 : 0,
                              (pde & PAGE_USER) ? 1 : 0,
                              (pde & PAGE_SIZE_4MB) ? 1 : 0);

              if (pde & PAGE_SIZE_4MB) {
                serial_printf(" Frame=%#lx)\n", (unsigned long)(pde & PAGING_PDE_ADDR_MASK_4MB));
              } else {
                  terminal_printf(" PT=0x%#lx)\n", (unsigned long)(pde & PAGING_PDE_ADDR_MASK_4KB));
              }
          }
      }
      terminal_write("------------------------\n");
 }

 

 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes)
 {
      terminal_write("[Paging Stage 3] Finalizing and activating paging...\n");
      if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) {
          PAGING_PANIC("Finalize: Invalid PD physical address!");
      }

      volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;

      uint32_t recursive_pde_flags = PAGE_PRESENT | PAGE_RW| PAGE_NX_BIT;
      pd_phys_ptr[RECURSIVE_PDE_INDEX] = (page_directory_phys & PAGING_ADDR_MASK) | recursive_pde_flags;
      terminal_printf("  Set recursive PDE[%u] to point to PD Phys=0x%#lx (Value=0x%lx)\n",
        (unsigned int)RECURSIVE_PDE_INDEX,
        (unsigned long)page_directory_phys,
        (unsigned long)pd_phys_ptr[RECURSIVE_PDE_INDEX]);

      serial_printf("  PD Entries Before Activation (Accessed via Phys Addr: 0x%#lx):\n", (unsigned long)page_directory_phys);
      debug_print_pd_entries((uint32_t*)pd_phys_ptr, 0x0, 4);
      debug_print_pd_entries((uint32_t*)pd_phys_ptr, KERNEL_SPACE_VIRT_START, 4);
      debug_print_pd_entries((uint32_t*)pd_phys_ptr, RECURSIVE_PDE_VADDR, 1);

      terminal_write("  Activating Paging (Loading CR3, Setting CR0.PG)...\n");
      paging_activate((uint32_t*)page_directory_phys);
      terminal_write("  Paging HW Activated.\n");

      uintptr_t kernel_pd_virt_addr = RECURSIVE_PD_VADDR;
      terminal_printf("  Setting global pointers: PD Virt=%p, PD Phys=0x%#lx\n",
        (void*)kernel_pd_virt_addr, (unsigned long)page_directory_phys);

      g_kernel_page_directory_phys = page_directory_phys;
      g_kernel_page_directory_virt = (uint32_t*)kernel_pd_virt_addr;

      terminal_printf("  Verifying recursive mapping via virtual access...\n");
      volatile uint32_t recursive_value_read_virt = g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX];
      terminal_printf("  Recursive PDE[%u] read via *Virt* Addr %p gives value: 0x%lx\n",
        (unsigned int)RECURSIVE_PDE_INDEX,
        (void*)&g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX],
        (unsigned long)recursive_value_read_virt);

      uint32_t actual_phys_in_pte = recursive_value_read_virt & PAGING_ADDR_MASK;
      uint32_t expected_phys = page_directory_phys & PAGING_ADDR_MASK;

      if (actual_phys_in_pte != expected_phys) {
          serial_printf("  ERROR: Recursive PDE verification failed!\n");
          terminal_printf("    Expected PD Phys: 0x%#lx\n", (unsigned long)expected_phys);
          terminal_printf("    Physical Addr in PDE read virtually: 0x%#lx\n", (unsigned long)actual_phys_in_pte);
          PAGING_PANIC("Failed to verify recursive mapping post-activation!");
      } else {
          terminal_printf("  Recursive mapping verified successfully.\n");
      }

      terminal_printf("  PD Entries After Activation (Accessed via Virt Addr: %p):\n", (void*)kernel_pd_virt_addr);
       debug_print_pd_entries(g_kernel_page_directory_virt, 0x0, 4);
       debug_print_pd_entries(g_kernel_page_directory_virt, KERNEL_SPACE_VIRT_START, 4);
       debug_print_pd_entries(g_kernel_page_directory_virt, RECURSIVE_PDE_VADDR, 1);

      terminal_write("[Paging Stage 3] Paging enabled and active. Higher half operational.\n");
      early_allocator_used = false;
      return 0; // Success
 }


 // Post-Activation Mapping Functions

 static int map_page_internal(uint32_t *target_page_directory_phys,
                             uintptr_t vaddr,
                             uintptr_t paddr,
                             uint32_t flags,
                             bool use_large_page)
{
    if (!g_kernel_page_directory_virt || g_kernel_page_directory_phys == 0) {
        KERNEL_PANIC_HALT("map_page_internal called before paging fully active and globals set!");
        return -1;
    }
    if (!target_page_directory_phys || ((uintptr_t)target_page_directory_phys % PAGE_SIZE) != 0) {
        terminal_printf("[Map Internal] Invalid target PD phys %#lx\n", (unsigned long)target_page_directory_phys);
        return KERN_EINVAL;
    }

    const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                      PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                      PAGE_SIZE_4MB | PAGE_GLOBAL |
                                      PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3 | PAGE_NX_BIT;
    uint32_t effective_flags = flags & VALID_FLAGS_MASK;

    bool is_current_pd = ((uintptr_t)target_page_directory_phys == g_kernel_page_directory_phys);

    uintptr_t aligned_vaddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(vaddr) : PAGE_ALIGN_DOWN(vaddr);
    uintptr_t aligned_paddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(paddr) : PAGE_ALIGN_DOWN(paddr);

    uint32_t pd_idx = PDE_INDEX(aligned_vaddr);

    if (pd_idx == RECURSIVE_PDE_INDEX) {
        terminal_printf("[Map Internal] Error: Attempted to map into recursive Paging range (V=%p, PDE %lu).\n",
                        (void*)vaddr, (unsigned long)pd_idx);
        return KERN_EPERM;
    }

    uint32_t base_flags = PAGE_PRESENT;
    if (effective_flags & PAGE_RW)   base_flags |= PAGE_RW;
    if (effective_flags & PAGE_USER) base_flags |= PAGE_USER;
    if (effective_flags & PAGE_PWT)  base_flags |= PAGE_PWT;
    if (effective_flags & PAGE_PCD)  base_flags |= PAGE_PCD;

    uint32_t pde_final_flags = 0;
    uint32_t pte_final_flags = 0;

    if (use_large_page) {
        if (!g_pse_supported) {
            terminal_printf("[Map Internal] Error: Attempted 4MB map, but PSE not supported/enabled.\n");
            return KERN_EPERM;
        }
        pde_final_flags = base_flags | PAGE_SIZE_4MB;
        if (effective_flags & PAGE_ACCESSED) pde_final_flags |= PAGE_ACCESSED;
        if (effective_flags & PAGE_DIRTY)    pde_final_flags |= PAGE_DIRTY;
        if ((effective_flags & PAGE_GLOBAL) && !(effective_flags & PAGE_USER)) {
             pde_final_flags |= PAGE_GLOBAL;
        }
    } else {
        pte_final_flags = base_flags;
        if (effective_flags & PAGE_ACCESSED) pte_final_flags |= PAGE_ACCESSED;
        if (effective_flags & PAGE_DIRTY)    pte_final_flags |= PAGE_DIRTY;
        if ((effective_flags & PAGE_GLOBAL) && !(effective_flags & PAGE_USER)) {
             pte_final_flags |= PAGE_GLOBAL;
        }
        if ((effective_flags & PAGE_NX_BIT) && g_nx_supported) {
             pte_final_flags |= PAGE_NX_BIT;
        }

        pde_final_flags = base_flags;
        if (pte_final_flags & PAGE_RW)   pde_final_flags |= PAGE_RW;
        if (pte_final_flags & PAGE_USER) pde_final_flags |= PAGE_USER;
        if (pte_final_flags & PAGE_PWT)  pde_final_flags |= PAGE_PWT;
        if (pte_final_flags & PAGE_PCD)  pde_final_flags |= PAGE_PCD;
    }

    // --- Modify Page Directory / Page Table ---

    if (is_current_pd) {
        // --- Operate on CURRENT Page Directory (use recursive mapping) ---

        if (use_large_page) {
            uint32_t new_pde_val_4mb = (aligned_paddr & PAGING_PDE_ADDR_MASK_4MB) | pde_final_flags;
            uint32_t current_pde = g_kernel_page_directory_virt[pd_idx];

            if (current_pde & PAGE_PRESENT) {
                if (current_pde == new_pde_val_4mb) return 0;
                terminal_printf("[Map Internal] Error: PDE[%lu] already present (value 0x%lx), cannot map 4MB page at V=%p\n",
                (unsigned long)pd_idx, (unsigned long)current_pde, (void*)aligned_vaddr);
                return KERN_EEXIST;
            }
            g_kernel_page_directory_virt[pd_idx] = new_pde_val_4mb;
            paging_invalidate_page((void*)aligned_vaddr);
            return 0;

        } else {
            uint32_t pde = g_kernel_page_directory_virt[pd_idx];
            uint32_t pt_idx = PTE_INDEX(aligned_vaddr);
            uint32_t* pt_virt;
            uintptr_t pt_phys_addr = 0;
            bool pt_allocated_here = false;

            if (!(pde & PAGE_PRESENT)) {
                pt_phys_addr = frame_alloc();
                if (pt_phys_addr == 0) {
                    serial_printf("[Map Internal] Error: frame_alloc failed for PT for V=%p.\n", (void*)aligned_vaddr);
                    return KERN_ENOMEM;
                }
                pt_allocated_here = true;

                uint32_t pde_value_to_write = (pt_phys_addr & PAGING_ADDR_MASK)
                                            | (pde_final_flags & ~PAGE_SIZE_4MB)
                                            | PAGE_PRESENT;

                g_kernel_page_directory_virt[pd_idx] = pde_value_to_write;
                paging_invalidate_page((void*)aligned_vaddr);

                pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
                memset(pt_virt, 0, PAGE_SIZE);

            } else if (pde & PAGE_SIZE_4MB) {
                 terminal_printf("[Map Internal] Error: Attempted 4KB map over existing 4MB page at V=%p (PDE[%lu]=0x%lx)\n",
                 (void*)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde);
                 return KERN_EPERM;
            } else {
                pt_phys_addr = pde & PAGING_ADDR_MASK;
                pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));

                uint32_t needed_pde_flags = pde_final_flags & ~PAGE_SIZE_4MB;
                uint32_t current_pde_flags = pde & (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD);

                if ((current_pde_flags & needed_pde_flags) != needed_pde_flags) {
                     uint32_t promoted_pde_val = (pde & PAGING_ADDR_MASK)
                                               | current_pde_flags
                                               | needed_pde_flags
                                               | PAGE_PRESENT;
                     g_kernel_page_directory_virt[pd_idx] = promoted_pde_val;
                     paging_invalidate_page((void*)aligned_vaddr);
                }
            }

            uint32_t new_pte_val_4kb = (aligned_paddr & PAGING_ADDR_MASK) | pte_final_flags;

            if (pt_virt[pt_idx] & PAGE_PRESENT) {
                uint32_t existing_pte_val = pt_virt[pt_idx];
                uintptr_t existing_phys = existing_pte_val & PAGING_ADDR_MASK;

                if (existing_phys == (aligned_paddr & PAGING_ADDR_MASK)) {
                    if (existing_pte_val != new_pte_val_4kb) {
                        pt_virt[pt_idx] = new_pte_val_4kb;
                        paging_invalidate_page((void*)aligned_vaddr);
                    }
                    return 0;
                } else {
                     terminal_printf("[Map Internal] Error: PTE[%lu] already present for V=%p but maps to different P=0x%#lx (tried P=0x%#lx)\n",
                     (unsigned long)pt_idx, (void*)aligned_vaddr, (unsigned long)existing_phys, (unsigned long)aligned_paddr);
                     if (pt_allocated_here) {
                         put_frame(pt_phys_addr);
                         g_kernel_page_directory_virt[pd_idx] = 0;
                         paging_invalidate_page((void*)aligned_vaddr);
                     }
                     return KERN_EEXIST;
                }
            }

            pt_virt[pt_idx] = new_pte_val_4kb;
            paging_invalidate_page((void*)aligned_vaddr);
            return 0;
        }

    } else {
        // --- Operate on NON-CURRENT Page Directory (use temporary mapping) ---
        int ret = -1;
        bool pt_allocated_here = false;
        uint32_t* target_pd_virt_temp = NULL;
        uint32_t* target_pt_virt_temp = NULL;
        uintptr_t pt_phys = 0;

        // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
        target_pd_virt_temp = paging_temp_map((uintptr_t)target_page_directory_phys, PTE_KERNEL_DATA_FLAGS);
        if (!target_pd_virt_temp) {
             serial_printf("[Map Internal] Error: Failed temp map DST PD %p\n", (void*)target_page_directory_phys);
             return -1;
        }

        uint32_t pde = target_pd_virt_temp[pd_idx];

        if (use_large_page) {
             uint32_t new_pde_val_4mb = (aligned_paddr & PAGING_PDE_ADDR_MASK_4MB) | pde_final_flags;
             if (pde & PAGE_PRESENT) {
                 terminal_printf("[Map Internal] Error: OTHER PD 4MB conflict at PDE[%lu] V=%p\n", (unsigned long)pd_idx, (void*)aligned_vaddr);
                 ret = KERN_EEXIST;
             } else {
                 target_pd_virt_temp[pd_idx] = new_pde_val_4mb;
                 ret = 0;
             }
             goto cleanup_other_pd;

        } else {
            if (pde & PAGE_PRESENT) {
                if (pde & PAGE_SIZE_4MB) {
                    serial_printf("[Map Internal] Error: OTHER PD 4KB conflict w 4MB at PDE[%lu] V=%p\n", (unsigned long)pd_idx, (void*)aligned_vaddr);
                    ret = KERN_EPERM;
                    goto cleanup_other_pd;
                }
                pt_phys = pde & PAGING_ADDR_MASK;
                uint32_t needed_pde_flags = pde_final_flags & ~PAGE_SIZE_4MB;
                uint32_t current_pde_flags = pde & (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD);
                if ((current_pde_flags & needed_pde_flags) != needed_pde_flags) {
                    uint32_t promoted_pde_val = (pde & PAGING_ADDR_MASK) | current_pde_flags | needed_pde_flags | PAGE_PRESENT;
                    target_pd_virt_temp[pd_idx] = promoted_pde_val;
                }
            } else {
                pt_phys = frame_alloc();
                if (!pt_phys) {
                    serial_printf("[Map Internal] Error: OTHER PD failed PT alloc for V=%p\n", (void*)aligned_vaddr);
                    ret = KERN_ENOMEM;
                    goto cleanup_other_pd;
                }
                pt_allocated_here = true;

                // FIX: Use paging_temp_map to map the new PT
                target_pt_virt_temp = paging_temp_map(pt_phys, PTE_KERNEL_DATA_FLAGS);
                if (!target_pt_virt_temp) {
                    terminal_printf("[Map Internal] Error: OTHER PD failed temp map new PT 0x%#lx\n", (unsigned long)pt_phys);
                    put_frame(pt_phys);
                    ret = -1;
                    goto cleanup_other_pd;
                }
                memset(target_pt_virt_temp, 0, PAGE_SIZE);
                // NOTE: PT remains mapped for PTE write below

                uint32_t pde_value_to_write = (pt_phys & PAGING_ADDR_MASK)
                                            | (pde_final_flags & ~PAGE_SIZE_4MB)
                                            | PAGE_PRESENT;
                target_pd_virt_temp[pd_idx] = pde_value_to_write;
            }

            // Map target PT if not already mapped from allocation
            if (!target_pt_virt_temp) {
                 target_pt_virt_temp = paging_temp_map(pt_phys, PTE_KERNEL_DATA_FLAGS); // Needs RW for PTE write
                 if (!target_pt_virt_temp) {
                     terminal_printf("[Map Internal] Error: OTHER PD failed temp map existing PT 0x%#lx for V=%p\n", (unsigned long)pt_phys, (void*)aligned_vaddr);
                     ret = -1;
                     goto cleanup_other_pd;
                 }
            }

            uint32_t pt_idx = PTE_INDEX(aligned_vaddr);
            uint32_t current_pte = target_pt_virt_temp[pt_idx];
            uint32_t new_pte_val_4kb = (aligned_paddr & PAGING_ADDR_MASK) | pte_final_flags;

            if (current_pte & PAGE_PRESENT) {
                uint32_t existing_phys = current_pte & PAGING_ADDR_MASK;
                if (existing_phys == (aligned_paddr & PAGING_ADDR_MASK)) {
                    if (current_pte != new_pte_val_4kb) {
                        target_pt_virt_temp[pt_idx] = new_pte_val_4kb;
                    }
                    ret = 0;
                } else {
                    terminal_printf("[Map Internal] Error: OTHER PD PTE[%lu] conflict for V=%p\n", (unsigned long)pt_idx, (void*)aligned_vaddr);
                    ret = KERN_EEXIST;
                    if (pt_allocated_here) {
                        target_pd_virt_temp[pd_idx] = 0;
                        put_frame(pt_phys);
                        if(target_pt_virt_temp) { // Check if PT is mapped before unmapping
                            paging_temp_unmap(target_pt_virt_temp);
                            target_pt_virt_temp = NULL;
                        }
                    }
                }
            } else {
                target_pt_virt_temp[pt_idx] = new_pte_val_4kb;
                ret = 0;
            }

            // Unmap the temporary PT mapping if it was used
            if (target_pt_virt_temp) {
                paging_temp_unmap(target_pt_virt_temp);
            }
        }

    cleanup_other_pd:
        // Unmap the temporary PD mapping
        if (target_pd_virt_temp) {
            paging_temp_unmap(target_pd_virt_temp);
        }
        return ret;
    }
}

 int paging_map_range(uint32_t *page_directory_phys,
                      uintptr_t virt_start_addr,
                      uintptr_t phys_start_addr,
                      size_t memsz,
                      uint32_t flags)
 {
      if (!page_directory_phys || memsz == 0) {
          serial_printf("[Map Range] Invalid arguments: PD=%p, size=%zu\n",
                          (void*)page_directory_phys, memsz);
          return -1;
      }

       const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                         PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                         PAGE_SIZE_4MB | PAGE_GLOBAL |
                                         PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3 | PAGE_NX_BIT;

      uint32_t masked_flags = flags & VALID_FLAGS_MASK;
      if (flags != masked_flags) {
           serial_printf("[Map Range] Warning: Input flags 0x%lx contained invalid bits. Using masked flags 0x%lx.\n", (unsigned long)flags, (unsigned long)masked_flags);
      }

      uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
      uintptr_t p_start = PAGE_ALIGN_DOWN(phys_start_addr);

      uintptr_t v_end;
      if (virt_start_addr > UINTPTR_MAX - memsz) {
          v_end = UINTPTR_MAX;
      } else {
          v_end = virt_start_addr + memsz;
      }
      v_end = PAGE_ALIGN_UP(v_end);

      if (v_end <= v_start) {
          if (memsz > 0 && (PAGE_ALIGN_UP(virt_start_addr + memsz) <= v_start)) {
              v_end = UINTPTR_MAX;
          } else {
              return 0;
          }
      }

      size_t total_size = (v_end > v_start) ? (v_end - v_start) : 0;
      const size_t MAX_SINGLE_MAPPING = 256 * 1024 * 1024;

      if (total_size > MAX_SINGLE_MAPPING) {
          serial_printf("[Map Range] Warning: Large mapping of %zu MB requested. Limiting to %zu MB for this call.\n",
                          (total_size / (1024*1024)),
                          (MAX_SINGLE_MAPPING / (1024*1024)));
          v_end = v_start + MAX_SINGLE_MAPPING;
          if (v_end < v_start) v_end = UINTPTR_MAX;
          v_end = PAGE_ALIGN_UP(v_end);
          if (v_end <= v_start) v_end = UINTPTR_MAX;
      }

      serial_printf("[Map Range] Mapping V=[0x%#lx-0x%#lx) to P=[0x%#lx...) Flags=0x%lx (Masked=0x%lx)\n",
        (unsigned long)v_start, (unsigned long)v_end, (unsigned long)p_start, (unsigned long)flags, (unsigned long)masked_flags);

      uintptr_t current_v = v_start;
      uintptr_t current_p = p_start;
      int mapped_pages = 0;
      int safety_counter = 0;
      const int max_map_loop_iterations = (MAX_SINGLE_MAPPING / PAGE_SIZE) + 10;

      while (current_v < v_end) {
          if (++safety_counter > max_map_loop_iterations) {
              serial_printf("[Map Range] Safety break after %d iterations\n", safety_counter);
              return -1;
          }

          size_t remaining_v_size = (v_end > current_v) ? (v_end - current_v) : 0;
          if (remaining_v_size == 0) {
              break;
          }

          bool possible_large = g_pse_supported &&
                                (current_v % PAGE_SIZE_LARGE == 0) &&
                                (current_p % PAGE_SIZE_LARGE == 0) &&
                                (remaining_v_size >= PAGE_SIZE_LARGE);

          bool use_large = false;

          if (possible_large) {
              uint32_t pd_idx_check = PDE_INDEX(current_v);
              uint32_t existing_pde_check = 0;

              if (g_kernel_page_directory_virt != NULL && (uintptr_t)page_directory_phys == g_kernel_page_directory_phys) {
                  existing_pde_check = g_kernel_page_directory_virt[pd_idx_check];
              } else if (g_kernel_page_directory_virt != NULL) {
                  serial_printf("[Map Range] Warning: Cannot check PDE for non-current PD %p. Forcing 4KB.\n", (void*)page_directory_phys);
                  existing_pde_check = PAGE_PRESENT;
              }

              if (!(existing_pde_check & PAGE_PRESENT)) {
                   use_large = true;
              } else {
                   use_large = false;
              }
          }

          int result = map_page_internal(page_directory_phys,
                                         current_v,
                                         current_p,
                                         masked_flags,
                                         use_large);

          if (result != 0) {
              serial_printf("[Map Range] Failed map_page_internal for V=0x%#lx P=0x%#lx Large=%d\n",
                (unsigned long)current_v, (unsigned long)current_p, use_large);
              return -1;
          }

          size_t step = use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;

          if (current_v > UINTPTR_MAX - step) {
              serial_printf("[Map Range] Virtual address overflow during iteration.\n");
              break;
          }
          uintptr_t next_p = current_p + step;
          if (next_p < current_p) {
              serial_printf("[Map Range] Warning: Physical address overflow during iteration.\n");
              break;
          }

          current_v += step;
          current_p = next_p;
          mapped_pages++;
      }

      serial_printf("[Map Range] Completed. Mapped %d pages/blocks for V=[0x%#lx - 0x%#lx).\n",
        mapped_pages, (unsigned long)v_start, (unsigned long)current_v);
      return 0;
 }

 // --- Utility Functions ---

 int paging_get_physical_address(uint32_t *page_directory_phys,
                                uintptr_t vaddr,
                                uintptr_t *paddr_out)
{
    if (!paddr_out) {
        serial_printf("[GetPhys] Error: Output pointer paddr_out is NULL.\n");
        return KERN_EINVAL;
    }
    *paddr_out = 0; // Default to 0 on failure or if not found

    bool is_current_pd = (g_kernel_page_directory_phys != 0 && (uintptr_t)page_directory_phys == g_kernel_page_directory_phys);

    if (is_current_pd) {
        // --- Lookup in CURRENT Page Directory ---
        if (!g_kernel_page_directory_virt) {
            terminal_printf("[GetPhys] Error: Lookup in current PD, but kernel PD virt pointer is NULL.\n");
            // This likely means paging isn't fully active or initialized correctly.
            return KERN_EPERM; // Indicate a system state error
        }

        uint32_t pd_idx = PDE_INDEX(vaddr);
        uint32_t pde = g_kernel_page_directory_virt[pd_idx];

        // Check if Page Directory Entry is present
        if (!(pde & PAGE_PRESENT)) {
            // terminal_printf("[GetPhys] Debug: PDE[%u] not present for V=%p in current PD.\n", pd_idx, (void*)vaddr);
            return KERN_ENOENT; // Page directory entry not present -> address not mapped
        }

        // Check if it's a 4MB page
        if (pde & PAGE_SIZE_4MB) {
            uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB; // Mask for 4MB page frame address
            uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1);    // Offset within the 4MB page
            *paddr_out = page_base_phys + page_offset;
            // serial_printf("[GetPhys] Debug: V=%p -> P=%p (4MB Page PDE[%u]=%#08lx)\n", (void*)vaddr, (void*)*paddr_out, pd_idx, pde);
            return 0; // Success
        } else {
            // It's a 4KB page, need to look into the Page Table
            // Access Page Table using recursive mapping
            uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
            uint32_t pt_idx = PTE_INDEX(vaddr);
            uint32_t pte = pt_virt[pt_idx];

            // Check if Page Table Entry is present
            if (!(pte & PAGE_PRESENT)) {
                // serial_printf("[GetPhys] Debug: PTE[%u][%u] not present for V=%p in current PD.\n", pd_idx, pt_idx, (void*)vaddr);
                return KERN_ENOENT; // Page table entry not present -> address not mapped
            }

            // Calculate final physical address
            uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK; // Mask for 4KB page frame address
            uintptr_t page_offset = vaddr & (PAGE_SIZE - 1);       // Offset within the 4KB page
            *paddr_out = page_base_phys + page_offset;
            // serial_printf("[GetPhys] Debug: V=%p -> P=%p (4KB Page PTE[%u][%u]=%#08lx)\n", (void*)vaddr, (void*)*paddr_out, pd_idx, pt_idx, pte);
            return 0; // Success
        }
    }
    else {
        // --- Lookup in ANOTHER Page Directory ---
        if (!page_directory_phys) {
             serial_printf("[GetPhys] Error: NULL page_directory_phys provided for non-current lookup.\n");
             return KERN_EINVAL;
        }
        // Need temporary mapping functions and kernel PD to be ready
        if (!g_kernel_page_directory_virt) {
             terminal_printf("[GetPhys] Error: Kernel PD Virt not available for temporary mapping.\n");
             return KERN_EPERM;
        }

        int ret = KERN_ENOENT; // Default to not found
        uint32_t* target_pd_virt_temp = NULL;
        uint32_t* target_pt_virt_temp = NULL;

        // Map the target Page Directory temporarily (read-only is sufficient)
        target_pd_virt_temp = paging_temp_map((uintptr_t)page_directory_phys, PTE_KERNEL_READONLY_FLAGS);
        if (!target_pd_virt_temp) {
            terminal_printf("[GetPhys] Error: Failed to temp map target PD P=%p.\n", (void*)page_directory_phys);
            return KERN_EPERM; // Indicate mapping failure
        }

        uint32_t pd_idx = PDE_INDEX(vaddr);
        uint32_t pde = target_pd_virt_temp[pd_idx];

        if (pde & PAGE_PRESENT) {
            if (pde & PAGE_SIZE_4MB) {
                // 4MB page case
                uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB;
                uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1);
                *paddr_out = page_base_phys + page_offset;
                // terminal_printf("[GetPhys] Debug: V=%p -> P=%p (Other PD, 4MB Page PDE[%u]=%#08lx)\n", (void*)vaddr, (void*)*paddr_out, pd_idx, pde);
                ret = 0; // Success
            } else {
                // 4KB page case - need to map the Page Table
                uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;
                target_pt_virt_temp = paging_temp_map(pt_phys, PTE_KERNEL_READONLY_FLAGS);

                if (!target_pt_virt_temp) {
                     serial_printf("[GetPhys] Error: Failed to temp map target PT P=%#lx from PD P=%p.\n", (unsigned long)pt_phys, (void*)page_directory_phys);
                     ret = KERN_EPERM; // Indicate mapping failure
                } else {
                    uint32_t pt_idx = PTE_INDEX(vaddr);
                    uint32_t pte = target_pt_virt_temp[pt_idx];

                    if (pte & PAGE_PRESENT) {
                        uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK;
                        uintptr_t page_offset = vaddr & (PAGE_SIZE - 1);
                        *paddr_out = page_base_phys + page_offset;
                        // terminal_printf("[GetPhys] Debug: V=%p -> P=%p (Other PD, 4KB Page PTE[%u][%u]=%#08lx)\n", (void*)vaddr, (void*)*paddr_out, pd_idx, pt_idx, pte);
                        ret = 0; // Success
                    } else {
                        // serial_printf("[GetPhys] Debug: PTE[%u][%u] not present for V=%p in other PD.\n", pd_idx, pt_idx, (void*)vaddr);
                        // ret remains KERN_ENOENT
                    }
                    // Unmap the temporary Page Table mapping
                    paging_temp_unmap(target_pt_virt_temp);
                }
            }
        } else {
             // serial_printf("[GetPhys] Debug: PDE[%u] not present for V=%p in other PD.\n", pd_idx, (void*)vaddr);
             // ret remains KERN_ENOENT
        }

        // Unmap the temporary Page Directory mapping
        paging_temp_unmap(target_pd_virt_temp);
        return ret;
    }
}

 // --- Process Management Support ---

 void paging_free_user_space(uint32_t *page_directory_phys) {
      if (!page_directory_phys || page_directory_phys == (uint32_t*)g_kernel_page_directory_phys) {
           serial_printf("[FreeUser] Error: Invalid or kernel PD provided (PD Phys: %p)\n", (void*)page_directory_phys);
          return;
      }
      if (!g_kernel_page_directory_virt) return;

      terminal_printf("[FreeUser] Freeing user space mappings for PD Phys %p\n", (void*)page_directory_phys);

      uint32_t* target_pd_virt_temp = NULL;

      // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
      target_pd_virt_temp = paging_temp_map((uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS);
      if (!target_pd_virt_temp) {
          terminal_printf("[FreeUser] Error: Failed to temp map target PD %p\n", (void*)page_directory_phys);
          return;
      }

      for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
          uint32_t pde = target_pd_virt_temp[i];

          if (pde & PAGE_PRESENT) {
              if (pde & PAGE_SIZE_4MB) {
                  uintptr_t frame_base = pde & PAGING_PDE_ADDR_MASK_4MB;
                  for (size_t f = 0; f < PAGES_PER_TABLE; ++f) {
                      uintptr_t frame_addr = frame_base + f * PAGE_SIZE;
                      if(frame_addr < frame_base) break;
                      put_frame(frame_addr);
                  }
                   terminal_printf("  Freed 4MB Page Frames for PDE[%zu]\n", i);
              } else {
                  uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;
                  uint32_t* target_pt_virt_temp = NULL;

                  // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
                  target_pt_virt_temp = paging_temp_map(pt_phys, PTE_KERNEL_DATA_FLAGS);
                  if (target_pt_virt_temp) {
                       for (size_t j = 0; j < PAGES_PER_TABLE; ++j) {
                           uint32_t pte = target_pt_virt_temp[j];
                           if (pte & PAGE_PRESENT) {
                               uintptr_t frame_phys = pte & PAGING_PTE_ADDR_MASK;
                               put_frame(frame_phys);
                           }
                       }
                       // FIX: Use paging_temp_unmap instead of paging_temp_unmap_vaddr
                       paging_temp_unmap(target_pt_virt_temp);
                   } else {
                       serial_printf("[FreeUser] Warning: Failed to temp map PT 0x%#lx from PDE[%zu] - frames leak!\n", (unsigned long)pt_phys, i);
                   }
                  put_frame(pt_phys);
              }
              target_pd_virt_temp[i] = 0;
          }
      }

      // FIX: Use paging_temp_unmap instead of paging_temp_unmap_vaddr
      paging_temp_unmap(target_pd_virt_temp);
      serial_printf("[FreeUser] User space mappings cleared for PD Phys %p\n", (void*)page_directory_phys);
 }


 uintptr_t paging_clone_directory(uint32_t* src_pd_phys_addr) {
      if (!src_pd_phys_addr || !g_kernel_page_directory_virt) {
          terminal_printf("[CloneDir] Error: Invalid source PD or paging not active.\n");
          return 0;
      }

      uintptr_t new_pd_phys = paging_alloc_frame(false);
      if (!new_pd_phys) {
          terminal_printf("[CloneDir] Error: Failed to allocate frame for new PD.\n");
          return 0;
      }
      terminal_printf("[CloneDir] Cloning PD %p -> New PD 0x%#lx\n", (void*)src_pd_phys_addr, (unsigned long)new_pd_phys);

      uint32_t* src_pd_virt_temp = NULL;
      uint32_t* dst_pd_virt_temp = NULL;
      int error_occurred = 0;
      uintptr_t allocated_pt_phys[KERNEL_PDE_INDEX];
      int allocated_pt_count = 0;
      memset(allocated_pt_phys, 0, sizeof(allocated_pt_phys));

      // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
      src_pd_virt_temp = paging_temp_map((uintptr_t)src_pd_phys_addr, PTE_KERNEL_READONLY_FLAGS);
      if (!src_pd_virt_temp) {
          terminal_printf("[CloneDir] Error: Failed to map source PD %p.\n", (void*)src_pd_phys_addr);
          error_occurred = 1; goto cleanup_clone_err;
      }

      // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
      dst_pd_virt_temp = paging_temp_map(new_pd_phys, PTE_KERNEL_DATA_FLAGS);
      if (!dst_pd_virt_temp) {
          terminal_printf("[CloneDir] Error: Failed to map destination PD 0x%#lx.\n", (unsigned long)new_pd_phys);
          error_occurred = 1; goto cleanup_clone_err;
      }

      for (size_t i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++) {
          dst_pd_virt_temp[i] = g_kernel_page_directory_virt[i];
      }
      dst_pd_virt_temp[RECURSIVE_PDE_INDEX] = (new_pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT;

      for (size_t i = 0; i < KERNEL_PDE_INDEX; i++) {
          uint32_t src_pde = src_pd_virt_temp[i];
          if (!(src_pde & PAGE_PRESENT)) { dst_pd_virt_temp[i] = 0; continue; }

          if (src_pde & PAGE_SIZE_4MB) {
               dst_pd_virt_temp[i] = src_pde;
               uintptr_t frame_base = src_pde & PAGING_PDE_ADDR_MASK_4MB;
               for (size_t f = 0; f < PAGES_PER_TABLE; ++f) {
                   uintptr_t frame_addr_to_inc = frame_base + f * PAGE_SIZE;
                   if (frame_addr_to_inc < frame_base) break;
                   frame_incref(frame_addr_to_inc);
               }
              continue;
          }

          uintptr_t src_pt_phys = src_pde & PAGING_PDE_ADDR_MASK_4KB;
          uintptr_t dst_pt_phys = paging_alloc_frame(false);
          if (!dst_pt_phys) {
              terminal_printf("[CloneDir] Error: Failed to allocate new PT for PDE[%zu].\n", i);
              error_occurred = 1; goto cleanup_clone_err;
          }
          if ((size_t)allocated_pt_count < ARRAY_SIZE(allocated_pt_phys)) {
              allocated_pt_phys[allocated_pt_count++] = dst_pt_phys;
          } else {
               serial_printf("[CloneDir] Error: Exceeded allocated_pt_phys array size.\n");
               put_frame(dst_pt_phys);
               error_occurred = 1; goto cleanup_clone_err;
          }

          uint32_t* src_pt_virt_temp = NULL;
          uint32_t* dst_pt_virt_temp = NULL;

          // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
          src_pt_virt_temp = paging_temp_map(src_pt_phys, PTE_KERNEL_READONLY_FLAGS);
          if (!src_pt_virt_temp) {
              terminal_printf("[CloneDir] Error: Failed to map source PT 0x%#lx.\n", (unsigned long)src_pt_phys);
              error_occurred = 1; goto cleanup_clone_err;
          }

          // FIX: Use paging_temp_map instead of paging_temp_map_vaddr
          dst_pt_virt_temp = paging_temp_map(dst_pt_phys, PTE_KERNEL_DATA_FLAGS);
          if (!dst_pt_virt_temp) {
              terminal_printf("[CloneDir] Error: Failed to map destination PT 0x%#lx.\n", (unsigned long)dst_pt_phys);
              paging_temp_unmap(src_pt_virt_temp); // FIX: Use correct unmap function
              error_occurred = 1; goto cleanup_clone_err;
          }

          for (size_t j = 0; j < PAGES_PER_TABLE; j++) {
              uint32_t src_pte = src_pt_virt_temp[j];
              if (src_pte & PAGE_PRESENT) {
                  uintptr_t frame_phys = src_pte & PAGING_PTE_ADDR_MASK;
                  frame_incref(frame_phys);
                  dst_pt_virt_temp[j] = src_pte;
              } else {
                  dst_pt_virt_temp[j] = 0;
              }
          }

          paging_temp_unmap(dst_pt_virt_temp); // FIX: Use correct unmap function
          paging_temp_unmap(src_pt_virt_temp); // FIX: Use correct unmap function
          dst_pd_virt_temp[i] = (dst_pt_phys & PAGING_ADDR_MASK) | (src_pde & PAGING_FLAG_MASK);
      }

  cleanup_clone_err:
      if (src_pd_virt_temp) paging_temp_unmap(src_pd_virt_temp); // FIX: Use correct unmap function
      if (dst_pd_virt_temp) paging_temp_unmap(dst_pd_virt_temp); // FIX: Use correct unmap function

      if (error_occurred) {
          terminal_printf("[CloneDir] Error occurred. Cleaning up allocations...\n");
          for (int k = 0; k < allocated_pt_count; k++) {
              if (allocated_pt_phys[k] != 0) put_frame(allocated_pt_phys[k]);
          }
          if (new_pd_phys != 0) put_frame(new_pd_phys);
          return 0;
      }

      terminal_printf("[CloneDir] Successfully cloned PD to 0x%#lx\n", (unsigned long)new_pd_phys);
      return new_pd_phys;
 }


 // --- Page Fault Handler ---
 void page_fault_handler(registers_t *regs) { // <-- Use isr_frame_t
    serial_write("[PF] Enter C page_fault_handler\n");
    uintptr_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    if (!regs) { // <-- Use frame
        terminal_printf("\n--- PAGE FAULT (#PF) --- \n");
        terminal_printf(" FATAL ERROR: frame pointer is NULL in page_fault_handler!\n");
        KERNEL_PANIC_HALT("Page Fault with NULL frame structure");
        return;
    }

    uint32_t error_code = regs->err_code; // <-- Use frame

    // Decode error code bits
    bool non_present       = !(error_code & 0x1); // Corrected: 0 = Not Present
    bool write_fault       = (error_code & 0x2);
    bool user_mode         = (error_code & 0x4); // Fault occurred while CPL=3?
    bool reserved_bit      = (error_code & 0x8);
    bool instruction_fetch = (error_code & 0x10);

    pcb_t* current_process = get_current_process();
    uint32_t current_pid = current_process ? current_process->pid : (uint32_t)-1;

    terminal_printf("\n--- PAGE FAULT (#PF) ---\n");
    // Use %lu for pid (uint32_t), %p for address, %lx for error_code
    terminal_printf(" PID: %lu, Addr: %p, ErrCode: 0x%lx\n",
        (unsigned long)current_pid,
        (void*)fault_addr,
        (unsigned long)error_code);
    terminal_printf(" Details: %s, %s, %s, %s, %s\n",
                    non_present ? "Not-Present" : "Present(Protection)", // Corrected label
                    write_fault ? "Write" : "Read",
                    user_mode ? "User-Mode" : "Supervisor-Mode", // Clarified label
                    reserved_bit ? "Reserved-Bit-Set" : "Reserved-OK",
                    instruction_fetch ? (g_nx_supported ? "Instruction-Fetch(NX?)" : "Instruction-Fetch") : "Data-Access");
    // Use %p for EIP, %lx for CS/EFLAGS
    terminal_printf(" CPU State: EIP=%p, CS=0x%lx, EFLAGS=0x%lx\n",
        (void*)regs->eip, // <-- Use frame
        (unsigned long)regs->cs, // <-- Use frame
        (unsigned long)regs->eflags); // <-- Use frame
    // Use %lx for general regs, %p for EBP, %lx for saved ESP
    terminal_printf(" EAX=0x%#lx EBX=0x%#lx ECX=0x%#lx EDX=0x%#lx\n",
            (unsigned long)regs->eax, (unsigned long)regs->ebx, // <-- Use frame
            (unsigned long)regs->ecx, (unsigned long)regs->edx); // <-- Use frame
    terminal_printf(" ESI=0x%#lx EDI=0x%#lx EBP=%p K_ESP_before_pusha=0x%#lx\n", // Renamed esp_dummy label
            (unsigned long)regs->esi, (unsigned long)regs->edi, // <-- Use frame
            (void*)regs->ebp, (unsigned long)regs->esp_dummy); // <-- Use frame

    // Conditionally access and print user_ss/user_esp
    // Check if the fault happened *while CPL was 3* by checking RPL of CS selector pushed by CPU
    if ((regs->cs & 3) != 0) { // RPL = bits 0-1 of CS selector <-- Use frame
        // If yes, user_ss and user_esp were pushed by the CPU *after* eflags
        // Access them directly from the frame struct
        terminal_printf("           U_ESP=0x%#lx, U_SS=0x%#lx\n",
                        (unsigned long)regs->useresp, // <-- Use frame
                        (unsigned long)regs->ss);     // <-- Use frame
    }

    if (!user_mode) { // Check error code flag: Did the fault *occur* while CPL=0?
        terminal_printf(" Reason: Fault occurred in Supervisor Mode!\n");
        if (reserved_bit) terminal_printf(" CRITICAL: Reserved bit set in paging structure accessed by kernel at VAddr %p!\n", (void*)fault_addr);

        // *** Corrected Logic Here ***
        if (non_present) { // Check if page was NOT present
             terminal_printf(" CRITICAL: Kernel attempted to access non-present page at VAddr %p!\n", (void*)fault_addr);
             // Line 1598 is likely here or just after
             KERNEL_PANIC_HALT("Irrecoverable Supervisor Page Fault");
        } else { // Page was present, so it's a protection fault
             if (write_fault) terminal_printf(" CRITICAL: Kernel write attempt caused protection fault at VAddr %p!\n", (void*)fault_addr);
             else terminal_printf(" CRITICAL: Kernel read/execute attempt caused protection fault at VAddr %p!\n", (void*)fault_addr);
             KERNEL_PANIC_HALT("Irrecoverable Supervisor Protection Fault");
        }
        // KERNEL_PANIC_HALT should not return
        return; // Keep compiler happy
    }

    // --- User Mode Fault ---
    terminal_printf(" Reason: Fault occurred in User Mode.\n");

    if (!current_process) {
        terminal_printf("  Error: No current process available for user fault! Addr=%p\n", (void*)fault_addr);
        PAGING_PANIC("User Page Fault without process context!");
        return;
    }

    if (!current_process->mm) {
        terminal_printf("  Error: Current process (PID %lu) has no mm_struct! Addr=%p\n",
                        (unsigned long)current_pid, (void*)fault_addr);
        goto kill_process;
    }

    mm_struct_t *mm = current_process->mm;
    if (!mm->pgd_phys) {
        terminal_printf("  Error: Current process mm_struct has no page directory (pgd_phys is NULL)!\n");
        goto kill_process;
    }

    if (reserved_bit) {
        terminal_printf("  Error: Reserved bit set in user-accessed page table entry. Corrupted mapping? Terminating.\n");
        goto kill_process;
    }

    // NX check relies on g_nx_supported flag and instruction_fetch bit from error code
    if (g_nx_supported && instruction_fetch) {
         terminal_printf("  Error: Instruction fetch from a No-Execute page (NX violation) at Addr=%p. Terminating.\n", (void*)fault_addr);
        goto kill_process;
    }

    terminal_printf("  Searching for VMA covering faulting address %p...\n", (void*)fault_addr);
    vma_struct_t *vma = find_vma(mm, fault_addr);

    if (!vma) {
        terminal_printf("  Error: No VMA covers the faulting address %p. Segmentation Fault.\n",
                        (void*)fault_addr);
        goto kill_process;
    }

    terminal_printf("  VMA Found: [%#lx - %#lx) Flags: %c%c%c PageProt: 0x%lx\n",
        (unsigned long)vma->vm_start, (unsigned long)vma->vm_end,
        (vma->vm_flags & VM_READ) ? 'R' : '-',
        (vma->vm_flags & VM_WRITE) ? 'W' : '-',
        (vma->vm_flags & VM_EXEC) ? 'X' : '-',
        (unsigned long)vma->page_prot);

    // Check permissions against VMA flags (not just PTE flags)
    if (write_fault && !(vma->vm_flags & VM_WRITE)) {
        terminal_printf("  Error: Write attempt to VMA without VM_WRITE flag. Segmentation Fault.\n");
        goto kill_process;
    }
    // Note: Read implies execute if NX is not supported/set
    if (!write_fault && !(vma->vm_flags & VM_READ)) {
         // This case might need refinement depending on execute handling
         terminal_printf("  Error: Read/Execute attempt from VMA without VM_READ flag. Segmentation Fault.\n");
         goto kill_process;
    }
     // Explicit execute check (relevant even without HW NX if VM_EXEC is used meaningfully)
     if (instruction_fetch && !(vma->vm_flags & VM_EXEC)) {
         // We might reach here even if HW NX isn't supported/active, but the VMA lacks EXEC permission
         terminal_printf("  Error: Instruction fetch from VMA without VM_EXEC flag. Segmentation Fault.\n");
         goto kill_process;
     }

    terminal_printf("  Attempting to handle fault via VMA operations (Demand Paging / COW)...\n");
    int handle_result = handle_vma_fault(mm, vma, fault_addr, error_code);

    if (handle_result == 0) {
        terminal_printf("  VMA fault handler succeeded. Resuming process PID %lu.\n", (unsigned long)current_pid);
        terminal_printf("--------------------------\n");
        return; // Resume process
    } else {
        terminal_printf("  Error: handle_vma_fault failed with code %d. Terminating process.\n", handle_result);
        goto kill_process;
    }

kill_process:
    terminal_printf("--- Unhandled User Page Fault ---\n");
    terminal_printf(" Terminating Process PID %lu.\n", (unsigned long)current_pid);
    terminal_printf("--------------------------\n");

    if (current_process) {
        remove_current_task_with_code(0xDEAD000E); // Use a distinct exit code for page faults
    } else {
        // This case should ideally not be reached if initial checks are done
        PAGING_PANIC("Page fault kill attempt with no valid process context!");
    }
    // remove_current_task_with_code should not return
    PAGING_PANIC("remove_current_task returned after page fault kill!");
}

 /*
  * Removed the stray code block that was previously here (lines 2120-2204 in original).
  */

 /**
  * @brief Checks if all entries in a given Page Table are zero.
  * @param pt_virt Virtual address of the Page Table to check.
  * @return true if all PTEs are 0, false otherwise.
  */
 static bool is_page_table_empty(uint32_t *pt_virt) {
     if (!pt_virt) {
         terminal_printf("[is_page_table_empty] Warning: NULL page table pointer provided\n");
         return true;
     }
     for (size_t i = 0; i < PAGES_PER_TABLE; ++i) {
         if (pt_virt[i] != 0) {
             return false;
         }
     }
     return true;
 }

 // --- TLB Flushing ---

 void tlb_flush_range(void* start_vaddr, size_t size) {
      uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start_vaddr);
      uintptr_t end_addr;

      if ((uintptr_t)start_vaddr > UINTPTR_MAX - size) {
          end_addr = UINTPTR_MAX;
      } else {
          end_addr = (uintptr_t)start_vaddr + size;
      }
      end_addr = PAGE_ALIGN_UP(end_addr);
      if (end_addr < addr) {
          end_addr = UINTPTR_MAX;
      }

      while (addr < end_addr) {
          paging_invalidate_page((void*)addr);
          if (addr > UINTPTR_MAX - PAGE_SIZE) {
              break;
          }
          addr += PAGE_SIZE;
      }
 }

 // --- Global PD Pointer Setup ---
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
      if (!pd_virt || !pd_phys) {
           serial_printf("[PagingSet] Error: Invalid null pointers provided.\n");
           return;
      }
      serial_printf("[PagingSet] Setting Kernel PD Globals: Virt=%p Phys=%#lx\n", (void*)pd_virt, (unsigned long)pd_phys);
      g_kernel_page_directory_virt = pd_virt;
      g_kernel_page_directory_phys = pd_phys;
 }

 int paging_map_single_4k(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
      if ((vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) {
          terminal_printf("[MapSingle4k] Error: Unaligned addresses V=%#lx P=%#lx\n", (unsigned long)vaddr, (unsigned long)paddr);
          return -1;
      }
      return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
 }


// --- Dynamic Temporary Mapping State ---
static spinlock_t g_temp_va_lock;
static uint32_t g_temp_va_bitmap[KERNEL_TEMP_MAP_COUNT / 32];
static bool g_temp_va_initialized = false;

// Helper function to set/clear/test bits in the bitmap
static inline void bitmap_set(uint32_t* map, int bit) {
    map[bit / 32] |= (1 << (bit % 32));
}
static inline void bitmap_clear(uint32_t* map, int bit) {
    map[bit / 32] &= ~(1 << (bit % 32));
}
static inline bool bitmap_test(uint32_t* map, int bit) {
    return (map[bit / 32] & (1 << (bit % 32))) != 0;
}

int paging_temp_map_init(void) {
    terminal_write("[Paging TempVA] Initializing dynamic temporary mapping allocator...\n");
    spinlock_init(&g_temp_va_lock);
    memset(g_temp_va_bitmap, 0, sizeof(g_temp_va_bitmap));
    g_temp_va_initialized = true;
    terminal_printf("  Temp VA Range: [%p - %p), Slots: %u\n",
                    (void*)KERNEL_TEMP_MAP_START, (void*)KERNEL_TEMP_MAP_END, KERNEL_TEMP_MAP_COUNT);
    return 0;
}

static uintptr_t temp_vaddr_alloc(void) {
    if (!g_temp_va_initialized) {
        KERNEL_PANIC_HALT("Temporary VA allocator used before initialization!");
    }

    // ---> FIX #2: Add Lock <---
    // Requires functions like spinlock_acquire_irqsave from your spinlock.h
    // uint32_t irq_flags = spinlock_acquire_irqsave(&g_temp_va_lock);
    uintptr_t allocated_vaddr = 0;

    // FIX: Use unsigned int for loop variable matching KERNEL_TEMP_MAP_COUNT type
    for (unsigned int i = 0; i < KERNEL_TEMP_MAP_COUNT; ++i) {
        if (!bitmap_test(g_temp_va_bitmap, i)) {
            bitmap_set(g_temp_va_bitmap, i);
            allocated_vaddr = KERNEL_TEMP_MAP_START + (i * PAGE_SIZE);
            break;
        }
    }

    // ---> FIX #2: Release Lock <---
    // Requires functions like spinlock_release_irqrestore from your spinlock.h
    // spinlock_release_irqrestore(&g_temp_va_lock, irq_flags);

    if (allocated_vaddr == 0) {
        terminal_printf("[TempVA Alloc] Warning: Out of temporary virtual addresses!\n");
    }
    return allocated_vaddr;
}

static void temp_vaddr_free(uintptr_t vaddr) {
     if (!g_temp_va_initialized) return;

    if (vaddr < KERNEL_TEMP_MAP_START || vaddr >= KERNEL_TEMP_MAP_END || (vaddr % PAGE_SIZE != 0)) {
        terminal_printf("[TempVA Free] Warning: Attempt to free invalid or out-of-range address %p\n", (void*)vaddr);
        return;
    }

    int bit = (vaddr - KERNEL_TEMP_MAP_START) / PAGE_SIZE;

    // ---> FIX #2: Add Lock <---
    // Requires functions like spinlock_acquire_irqsave from your spinlock.h
    // uint32_t irq_flags = spinlock_acquire_irqsave(&g_temp_va_lock);

    if (!bitmap_test(g_temp_va_bitmap, bit)) {
        terminal_printf("[TempVA Free] Warning: Double free detected for address %p (bit %d)\n", (void*)vaddr, bit);
    } else {
        bitmap_clear(g_temp_va_bitmap, bit);
    }

    // ---> FIX #2: Release Lock <---
    // Requires functions like spinlock_release_irqrestore from your spinlock.h
    // spinlock_release_irqrestore(&g_temp_va_lock, irq_flags);
}


void* paging_temp_map(uintptr_t phys_addr, uint32_t flags) {
    if (!g_kernel_page_directory_virt) {
        terminal_printf("[TempMap] Error: Kernel PD not ready.\n");
        return NULL;
    }
    if (phys_addr % PAGE_SIZE != 0) {
        terminal_printf("[TempMap] Error: Physical address %#lx not page-aligned.\n", phys_addr);
        return NULL;
    }

    uintptr_t vaddr = temp_vaddr_alloc();
    if (vaddr == 0) return NULL;

    uint32_t pd_idx = PDE_INDEX(vaddr);
    uint32_t pde = g_kernel_page_directory_virt[pd_idx];

    if (!(pde & PAGE_PRESENT)) {
        uintptr_t pt_phys = frame_alloc();
        if (pt_phys == 0) {
            terminal_printf("[TempMap] Error: Failed to allocate PT frame for V=%p.\n", (void*)vaddr);
            temp_vaddr_free(vaddr);
            return NULL;
        }
        // FIX: Check return value of recursive call
        void* temp_pt_map = paging_temp_map(pt_phys, PTE_KERNEL_DATA_FLAGS);
        if (!temp_pt_map) {
             terminal_printf("[TempMap] Error: Failed to temp map new PT frame %#lx for zeroing.\n", pt_phys);
             put_frame(pt_phys);
             temp_vaddr_free(vaddr);
             return NULL;
        }
        memset(temp_pt_map, 0, PAGE_SIZE);
        paging_temp_unmap(temp_pt_map);

        uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT;
        g_kernel_page_directory_virt[pd_idx] = (pt_phys & PAGING_ADDR_MASK) | pde_flags;
        paging_invalidate_page((void*)vaddr);
    } else if (pde & PAGE_SIZE_4MB) {
        terminal_printf("[TempMap] Error: Temporary VA range V=%p overlaps with a 4MB kernel page!\n", (void*)vaddr);
        temp_vaddr_free(vaddr);
        return NULL;
    }

    uint32_t pt_idx = PTE_INDEX(vaddr);
    uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));

    if (pt_virt[pt_idx] & PAGE_PRESENT) {
        terminal_printf("[TempMap] CRITICAL Error: PTE[%lu] for dynamically allocated VA %p is already present (%#lx)!\n",
                        (unsigned long)pt_idx, (void*)vaddr, (unsigned long)pt_virt[pt_idx]); // Fixed format specifier
        temp_vaddr_free(vaddr);
        return NULL;
    }

    uint32_t final_flags = (flags | PAGE_PRESENT) & PAGING_FLAG_MASK;
    uint32_t new_pte = (phys_addr & PAGING_ADDR_MASK) | final_flags;
    pt_virt[pt_idx] = new_pte;

    paging_invalidate_page((void*)vaddr);

    return (void*)vaddr;
}

void paging_temp_unmap(void* temp_vaddr) {
    uintptr_t vaddr = (uintptr_t)temp_vaddr;

    if (vaddr < KERNEL_TEMP_MAP_START || vaddr >= KERNEL_TEMP_MAP_END || (vaddr % PAGE_SIZE != 0)) {
        serial_printf("[TempUnmap] Warning: Invalid or out-of-range temporary address %p provided for unmap.\n", temp_vaddr);
        return;
    }
    if (!g_kernel_page_directory_virt) {
        terminal_printf("[TempUnmap] Error: Kernel PD not ready.\n");
        return;
    }
    if (!g_temp_va_initialized) return;

    uint32_t pd_idx = PDE_INDEX(vaddr);
    uint32_t pt_idx = PTE_INDEX(vaddr);
    uint32_t pde = g_kernel_page_directory_virt[pd_idx];

    if (!(pde & PAGE_PRESENT) || (pde & PAGE_SIZE_4MB)) {
        terminal_printf("[TempUnmap] Warning: PDE for temp area invalid (PDE[%lu]=%#lx) during unmap of V=%p.\n",
                        (unsigned long)pd_idx, (unsigned long)pde, temp_vaddr);
    } else {
        uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
        if (pt_virt[pt_idx] & PAGE_PRESENT) {
            pt_virt[pt_idx] = 0;
            paging_invalidate_page(temp_vaddr);
        }
    }
    temp_vaddr_free(vaddr);
}

 // Helper for copying kernel PDEs
 /* paging.c -------------------------------------------------------------- */
void copy_kernel_pde_entries(uint32_t *dst_pd)
{
    if (!g_kernel_page_directory_virt || !dst_pd) { /* Basic null checks */
        KERNEL_PANIC_HALT("copy_kernel_pde_entries: Invalid PD pointers!");
        return;
    }

    /* --- FIX: Copy identity PDE[0] so early-mapped pages & IDT stay reachable --- */
    dst_pd[0] = g_kernel_page_directory_virt[0];
    /* --- END FIX --- */


    size_t start_index = KERNEL_PDE_INDEX;          /* 768 */
    size_t end_index   = RECURSIVE_PDE_INDEX;       /* 1023 */

    // ... rest of the function ...
    memcpy(dst_pd + start_index,
           g_kernel_page_directory_virt + start_index,
           (end_index - start_index) * sizeof(uint32_t));

    serial_printf("[CopyPDEs] Kernel PDE memcpy complete.\n");
}

int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz) {
    PAGING_DEBUG_PRINTF("Enter: V=[0x%#lx - 0x%#lx) in PD Phys %p",
        (unsigned long)virt_start_addr, (unsigned long)(virt_start_addr + memsz), (void*)page_directory_phys);

    // Basic validation of input parameters
    if (!page_directory_phys) {
        terminal_printf("[Unmap Range] Error: Invalid Page Directory pointer (NULL).\n");
        return -1;
    }
    // Handle zero size gracefully - it's not an error, just nothing to do.
    if (memsz == 0) {
        PAGING_DEBUG_PRINTF("Zero size requested, nothing to unmap.");
        return 0;
    }

    // Calculate page-aligned start and end virtual addresses
    uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
    uintptr_t v_end;

    // Check for potential overflow when calculating the end address
    if (virt_start_addr > UINTPTR_MAX - memsz) {
        v_end = UINTPTR_MAX; // Cap at maximum address if overflow occurs
    } else {
        v_end = virt_start_addr + memsz;
    }
    v_end = PAGE_ALIGN_UP(v_end); // Align the end address up to the next page boundary

    // Check for overflow after alignment or if start >= end
    if (v_end <= v_start) { // Use <= to catch zero size after alignment
        PAGING_DEBUG_PRINTF("Range is empty after alignment.");
        return 0;
    }

    // Determine if operating on the currently active page directory
    bool is_current_pd = ((uintptr_t)page_directory_phys == g_kernel_page_directory_phys);
    uint32_t* target_pd_virt = NULL; // Virtual address pointer to the target PD

    // Get a virtual pointer to the target page directory
    if (!is_current_pd) {
        // Map the target PD temporarily if it's not the current one
        target_pd_virt = paging_temp_map((uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS); // Map RW
        if (!target_pd_virt) {
            terminal_printf("[Unmap Range] Error: Failed to temp map target PD %p.\n", (void*)page_directory_phys);
            return -1; // Indicate failure
        }
    } else {
        // Use the globally mapped kernel PD if it's the target
        target_pd_virt = g_kernel_page_directory_virt;
        if (!target_pd_virt) {
             // This should not happen if paging is active
             KERNEL_PANIC_HALT("Unmap Range on current PD, but kernel PD virt is NULL!");
             return -1;
        }
    }

    int unmapped_count = 0; // Counter for pages successfully unmapped

    // Iterate through the virtual address range, processing PDE by PDE
    for (uintptr_t v_addr = v_start; v_addr < v_end; ) { // Increment is handled inside the loop
        uint32_t pd_idx = PDE_INDEX(v_addr);

        // --- FIXED WARNING: Use debug print for skipping kernel range ---
        // Skip kernel space and recursive mapping area
        if (pd_idx >= KERNEL_PDE_INDEX) {
            PAGING_DEBUG_PRINTF("Skipping unmap for kernel/recursive range V=0x%#lx.", (unsigned long)v_addr);
            // Calculate the start of the next PDE range (4MB boundary)
            uintptr_t next_v_addr = PAGE_ALIGN_DOWN(v_addr) + PAGE_SIZE_LARGE;
             // Check for overflow when calculating the next address
             if (next_v_addr <= v_addr) {
                 v_addr = v_end; // Reached end or overflowed, terminate loop
             } else {
                 v_addr = next_v_addr; // Advance to the next PDE boundary
             }
             continue; // Skip to the next iteration
        }
        // --- END FIX ---

        // Read the Page Directory Entry from the (potentially temporarily mapped) PD
        uint32_t pde = target_pd_virt[pd_idx];

        // If the PDE is not present, skip this 4MB range
        if (!(pde & PAGE_PRESENT)) {
             PAGING_DEBUG_PRINTF("PDE[%u] for V=0x%#lx not present, skipping.", pd_idx, (unsigned long)v_addr);
             uintptr_t next_v_addr = PAGE_ALIGN_DOWN(v_addr) + PAGE_SIZE_LARGE; // Skip to next PDE boundary
             if (next_v_addr <= v_addr) { v_addr = v_end; } else { v_addr = next_v_addr; }
             continue;
        }

        // This function cannot handle unmapping parts of a 4MB page
        if (pde & PAGE_SIZE_4MB) {
            terminal_printf("[Unmap Range] Error: Cannot unmap range overlapping 4MB page at V=0x%#lx (PDE[%u]=0x%lx).\n",
                            (unsigned long)v_addr, pd_idx, (unsigned long)pde);
            if (!is_current_pd) paging_temp_unmap(target_pd_virt); // Unmap temp PD if used
            return -1; // Indicate error
        }

        // PDE points to a 4KB Page Table
        uintptr_t pt_phys = pde & PAGING_ADDR_MASK; // Physical address of the Page Table
        uint32_t* pt_virt = NULL; // Virtual address pointer to the Page Table
        bool pt_mapped_here = false; // Flag if we mapped the PT temporarily
        bool pt_freed = false; // Flag if the PT was freed in this iteration

        // Get a virtual pointer to the Page Table
        if (is_current_pd) {
             // Use recursive mapping for the current PD
             pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
        } else {
             // Map the target PT temporarily
             pt_virt = paging_temp_map(pt_phys, PTE_KERNEL_DATA_FLAGS); // Map RW
             if (!pt_virt) {
                 terminal_printf("[Unmap Range] Error: Failed to temp map target PT %#lx for V=0x%#lx.\n", (unsigned long)pt_phys, (unsigned long)v_addr);
                 if (!is_current_pd) paging_temp_unmap(target_pd_virt); // Unmap temp PD
                  return -1; // Indicate failure
             }
             pt_mapped_here = true; // Mark that we mapped it
        }

        // Process pages within this Page Table until the end of the requested range
        // or the end of the 4MB covered by this PT, whichever comes first.
        uintptr_t pt_range_end = MIN(v_end, PAGE_ALIGN_DOWN(v_addr) + PAGE_SIZE_LARGE);
        PAGING_DEBUG_PRINTF("Processing PT (Phys %#lx, Virt %p) for V=[%p - %p)",
                            (unsigned long)pt_phys, pt_virt, (void*)v_addr, (void*)pt_range_end);

        while (v_addr < pt_range_end) {
            uint32_t pt_idx = PTE_INDEX(v_addr); // Index within the current Page Table
            uint32_t pte = pt_virt[pt_idx]; // Read the Page Table Entry

            // If the PTE is present, unmap the page
            if (pte & PAGE_PRESENT) {
                uintptr_t frame_phys = pte & PAGING_ADDR_MASK; // Physical address of the frame
                PAGING_DEBUG_PRINTF("  Unmapping V=%p (PTE[%u]=0x%lx -> P=%#lx)",
                                    (void*)v_addr, pt_idx, (unsigned long)pte, (unsigned long)frame_phys);
                pt_virt[pt_idx] = 0; // Clear the PTE
                paging_invalidate_page((void*)v_addr); // Invalidate TLB for this virtual address
                put_frame(frame_phys); // Free the underlying physical frame
                unmapped_count++; // Increment count of unmapped pages
            }

            // Check for overflow before incrementing the virtual address
            if (v_addr > UINTPTR_MAX - PAGE_SIZE) {
                PAGING_DEBUG_PRINTF("Virtual address overflow during inner loop.");
                v_addr = UINTPTR_MAX; // Prevent overflow and ensure loop termination
                break; // Exit the inner loop
            }
            v_addr += PAGE_SIZE; // Move to the next page address
        } // End inner loop (pages within the current PT)


        // Check if the Page Table is now empty *after* processing pages within it
        uintptr_t start_addr_of_pt_range = PAGE_ALIGN_DOWN(v_addr > PAGE_SIZE ? v_addr - PAGE_SIZE : 0);
        if (pt_range_end > start_addr_of_pt_range) { // Check if we actually processed pages in this PT
            if (is_page_table_empty(pt_virt)) {
                PAGING_DEBUG_PRINTF("PT at Phys 0x%#lx (PDE[%lu]) became empty. Freeing PT.",
                                    (unsigned long)pt_phys, (unsigned long)pd_idx);
                target_pd_virt[pd_idx] = 0; // Clear the PDE in the target PD
                // Invalidate TLB for an address covered by this PDE
                paging_invalidate_page((void*)start_addr_of_pt_range);
                put_frame(pt_phys); // Free the page table frame itself
                pt_freed = true; // Mark that the PT was freed
            }
        }

        // Unmap the temporary PT mapping if we created one
        if (pt_mapped_here) {
            paging_temp_unmap(pt_virt);
        }

        // v_addr is already advanced correctly by the inner loop

    } // End outer loop (virtual addresses / PDEs)

    // Unmap the temporary PD mapping if we created one
    if (!is_current_pd) {
        paging_temp_unmap(target_pd_virt);
    }

    PAGING_DEBUG_PRINTF("Finished. Unmapped approx %d pages.", unmapped_count);
    return 0; // Success
}




 int paging_get_physical_address_and_flags(uint32_t *page_directory_phys,
                                          uintptr_t vaddr,
                                          uintptr_t *paddr_out,
                                          uint32_t *flags_out)
{
    if (!paddr_out || !flags_out) {
        terminal_printf("[GetPhysFlags] Error: Output pointer paddr_out or flags_out is NULL.\n");
        return KERN_EINVAL;
    }
    *paddr_out = 0;
    *flags_out = 0; // Clear output flags on entry

    bool is_current_pd = (g_kernel_page_directory_phys != 0 && (uintptr_t)page_directory_phys == g_kernel_page_directory_phys);

    if (is_current_pd) {
        // --- Lookup in CURRENT Page Directory ---
        if (!g_kernel_page_directory_virt) {
            terminal_printf("[GetPhysFlags] Error: Lookup in current PD, but kernel PD virt pointer is NULL.\n");
            return KERN_EPERM;
        }

        uint32_t pd_idx = PDE_INDEX(vaddr);
        uint32_t pde = g_kernel_page_directory_virt[pd_idx];

        if (!(pde & PAGE_PRESENT)) return KERN_ENOENT;

        if (pde & PAGE_SIZE_4MB) {
            uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB;
            uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1);
            *paddr_out = page_base_phys + page_offset;
            *flags_out = pde & PAGING_FLAG_MASK; // Return PDE flags for 4MB page
            return 0; // Success
        } else {
            uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
            uint32_t pt_idx = PTE_INDEX(vaddr);
            uint32_t pte = pt_virt[pt_idx];

            if (!(pte & PAGE_PRESENT)) return KERN_ENOENT;

            uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK;
            uintptr_t page_offset = vaddr & (PAGE_SIZE - 1);
            *paddr_out = page_base_phys + page_offset;
            *flags_out = pte & PAGING_FLAG_MASK; // Return PTE flags for 4KB page
            return 0; // Success
        }
    }
    else {
        // --- Lookup in ANOTHER Page Directory ---
        if (!page_directory_phys) return KERN_EINVAL;
        if (!g_kernel_page_directory_virt) return KERN_EPERM;

        int ret = KERN_ENOENT;
        uint32_t* target_pd_virt_temp = NULL;
        uint32_t* target_pt_virt_temp = NULL;

        target_pd_virt_temp = paging_temp_map((uintptr_t)page_directory_phys, PTE_KERNEL_READONLY_FLAGS);
        if (!target_pd_virt_temp) {
            return KERN_EPERM;
        }

        uint32_t pd_idx = PDE_INDEX(vaddr);
        uint32_t pde = target_pd_virt_temp[pd_idx];

        if (pde & PAGE_PRESENT) {
            if (pde & PAGE_SIZE_4MB) {
                uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB;
                uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1);
                *paddr_out = page_base_phys + page_offset;
                *flags_out = pde & PAGING_FLAG_MASK; // Get flags from PDE
                ret = 0;
            } else {
                uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;
                target_pt_virt_temp = paging_temp_map(pt_phys, PTE_KERNEL_READONLY_FLAGS);
                if (!target_pt_virt_temp) {
                     ret = KERN_EPERM;
                } else {
                    uint32_t pt_idx = PTE_INDEX(vaddr);
                    uint32_t pte = target_pt_virt_temp[pt_idx];
                    if (pte & PAGE_PRESENT) {
                        uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK;
                        uintptr_t page_offset = vaddr & (PAGE_SIZE - 1);
                        *paddr_out = page_base_phys + page_offset;
                        *flags_out = pte & PAGING_FLAG_MASK; // Get flags from PTE
                        ret = 0;
                    }
                    paging_temp_unmap(target_pt_virt_temp); // Unmap temp PT
                }
            }
        }
        paging_temp_unmap(target_pd_virt_temp); // Unmap temp PD
        return ret;
    }
}

