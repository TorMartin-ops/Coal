/**
 * paging.c - Paging Implementation (32-bit x86 with PSE and NX via EFER)
 */

 #include "paging.h"
 #include "frame.h"              // Frame allocator (get_frame, put_frame, frame_alloc)
 #include "buddy.h"              // Buddy allocator (BUDDY_ALLOC, BUDDY_FREE)
 #include "terminal.h"           // Kernel console output
 #include "types.h"              // Core type definitions
 #include "process.h"            // For page fault context
 #include "mm.h"                 // For VMA handling during page faults
 #include "scheduler.h"          // For terminating process on critical fault
 #include "string.h"             // Kernel's memset, memcpy
 #include "cpuid.h"              // CPUID instruction wrapper
 #include "kmalloc_internal.h"   // For ALIGN_UP/DOWN macros
 #include "multiboot2.h"         // For parsing memory map
 #include "msr.h"                // For MSR read/write (EFER)
 
 // --- Constants and Macros ---
 #ifndef PAGING_PANIC
 #define PAGING_PANIC(msg) do { \
       terminal_printf("\n[PAGING PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
       while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 #endif
 
 #ifndef ARRAY_SIZE
 #define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
 #endif
 
 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL;
 uint32_t  g_kernel_page_directory_phys = 0;
 bool      g_pse_supported              = false;
 bool      g_nx_supported               = false;
 
 // --- Linker Symbols ---
 extern uint8_t _kernel_start_phys[];
 extern uint8_t _kernel_end_phys[];
 extern uint8_t _kernel_text_start_phys[];
 extern uint8_t _kernel_text_end_phys[];
 extern uint8_t _kernel_rodata_start_phys[];
 extern uint8_t _kernel_rodata_end_phys[];
 extern uint8_t _kernel_data_start_phys[];
 extern uint8_t _kernel_data_end_phys[];
 
 // --- Multiboot Info ---
 extern uint32_t g_multiboot_info_phys_addr_global;
 
 // --- Early Allocation Tracking ---
 #define MAX_EARLY_ALLOCATIONS 128
 static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
 static int       early_allocated_count  = 0;
 static bool      early_allocator_used   = false; // Tracks if early allocator is ACTIVE, becomes false after finalize
 
 // --- External Assembly Functions ---
 extern void paging_invalidate_page(void *vaddr);
 extern void paging_activate(uint32_t *page_directory_phys);
 
 // --- Forward Declarations (Internal Functions) ---
 static int          map_page_internal(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags, bool use_large_page);
 static inline void  enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void  write_cr4(uint32_t value);
 static bool         check_and_enable_nx(void);
 static uintptr_t    paging_alloc_frame(bool use_early_allocator);
 static uint32_t* allocate_page_table_phys(bool use_early_allocator);
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type);
 static int          kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
 static void         kernel_unmap_virtual_unsafe(uintptr_t vaddr);
 static int          paging_map_physical_early(uintptr_t page_directory_phys, uintptr_t phys_addr_start, size_t size, uint32_t flags, bool map_to_higher_half);
 static void         debug_print_pd_entries(uint32_t* pd_phys_ptr, uintptr_t vaddr_start, size_t count);
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
     if (!g_kernel_page_directory_virt) {
         // If called before paging is fully active and recursive mapping established,
         // this relies on the caller having another way to access the kernel PD.
         // After activation, g_kernel_page_directory_virt MUST be valid.
         terminal_printf("[KMapUnsafe] Warning: Kernel PD Virt not set (may be okay pre-activation)\n");
         // This function is problematic before full activation - needs careful use.
         // If needed pre-activation, direct physical access or identity map is safer.
         // For now, assume it's called post-activation or caller handles PD access.
         if (!g_kernel_page_directory_virt) return -1; // Hard fail if called post-activation without valid virt ptr
     }
 
     // Check alignment and range
     if ((vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) {
         terminal_printf("[KMapUnsafe] Error: Unaligned addresses V=0x%x P=0x%x\n", vaddr, paddr);
         return -1;
     }
 
     // Allow temporary mappings anywhere for flexibility, but user must be cautious.
     // The original check restricting to kernel space or specific TEMP addresses is removed,
     // but this makes the function more dangerous if misused.
     // terminal_printf("[KMapUnsafe] Mapping V=0x%x -> P=0x%x\n", vaddr, paddr);
 
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
 
     // Access kernel PD using its virtual address (ASSUMES PAGING ACTIVE or special setup)
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
 
     // If PDE not present, we need to create a new page table
     if (!(pde & PAGE_PRESENT)) {
         // Allocate a new page table using frame allocator (NOT early allocator here, assumes buddy is up)
         uintptr_t new_pt_phys = frame_alloc();
         if (new_pt_phys == 0) {
             terminal_printf("[KMapUnsafe] Error: Failed to allocate PT for VAddr 0x%x\n", vaddr);
             return -1;
         }
 
         // Map the PT in the page directory
         // Ensure flags don't include user bit unless explicitly intended
         g_kernel_page_directory_virt[pd_idx] = (new_pt_phys & ~0xFFF) | PAGE_PRESENT | PAGE_RW; // Kernel RW default
         paging_invalidate_page((void*)vaddr); // Invalidate old mapping potentially covering this vaddr range
 
         // Now we need to clear the new PT - use recursive mapping to access it
         uint32_t* new_pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
 
         // Clear the PT
         // Use memset for efficiency if available and mapped RW
          memset(new_pt_virt, 0, PAGE_SIZE);
         // Fallback loop if memset isn't safe/available
         // for (int i = 0; i < PAGES_PER_TABLE; i++) {
         //     new_pt_virt[i] = 0;
         // }
 
         // Now set the specific PTE entry
         new_pt_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
         paging_invalidate_page((void*)vaddr); // Invalidate the specific vaddr
         return 0;
     }
 
     // Check if this is a 4MB page
     if (pde & PAGE_SIZE_4MB) {
         terminal_printf("[KMapUnsafe] Error: Cannot map 4KB page into existing 4MB PDE V=0x%x\n", vaddr);
         return -1;
     }
 
     // PDE exists and points to a 4KB page table
     // Use recursive mapping to access the existing PT
     uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
 
     // Check if PTE already exists
     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         uintptr_t existing_paddr = pt_virt[pt_idx] & ~0xFFF;
         if (existing_paddr != (paddr & ~0xFFF)) {
              terminal_printf("[KMapUnsafe] Warning: Overwriting existing PTE for V=0x%x (Old P=0x%x, New P=0x%x)\n",
                             vaddr, existing_paddr, paddr);
         }
         // Allow overwriting identical mapping or changing flags
     }
 
     // Set the PTE entry
     pt_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     paging_invalidate_page((void*)vaddr);
     return 0;
 } // *** END OF kernel_map_virtual_to_physical_unsafe ***
 
 // Marked static as it's an internal unsafe helper
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr) {
     if (!g_kernel_page_directory_virt) {
         terminal_printf("[KUnmapUnsafe] Error: Kernel PD Virt not set!\n");
         return;
     }
     // Basic alignment check
     if (vaddr % PAGE_SIZE != 0) {
         terminal_printf("[KUnmapUnsafe] Error: VAddr 0x%x not page aligned.\n", vaddr);
         return;
     }
 
     // Allow unmapping any address, caller beware.
     // Original check restricted to kernel space or temp areas.
     // terminal_printf("[KUnmapUnsafe] Unmapping V=0x%x\n", vaddr);
 
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
 
     // Access kernel PD via virtual address
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
 
     // If PDE not present or is a 4MB page, we can't unmap a 4KB page via PTE
     if (!(pde & PAGE_PRESENT)) {
         // terminal_printf("[KUnmapUnsafe] PDE not present for V=0x%x\n", vaddr);
         return; // Nothing to unmap at PDE level
     }
     if (pde & PAGE_SIZE_4MB) {
         terminal_printf("[KUnmapUnsafe] Cannot unmap 4KB page within a 4MB PDE V=0x%x\n", vaddr);
         return; // Cannot unmap a 4k portion of a 4M page this way
     }
 
     // PDE points to a 4KB page table. Access it via recursive mapping.
     uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
 
     // Check if the PTE is actually present before clearing
     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         pt_virt[pt_idx] = 0; // Clear the PTE
         paging_invalidate_page((void*)vaddr); // Invalidate the TLB for the virtual address
     } else {
         // terminal_printf("[KUnmapUnsafe] PTE not present for V=0x%x\n", vaddr);
     }
 
     // NOTE: This function does NOT free the frame the PTE pointed to,
     // nor does it check if the page table becomes empty and can be freed.
     // It's a low-level "zap PTE" function.
 }
 
 // --- Early Memory Allocation ---
 // Marked static as it's an internal early boot helper
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
     // Access MB info directly via physical addr (ASSUMES <1MB or identity mapped)
     // Need volatile as memory contents can change unexpectedly before caching/MMU setup.
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Basic sanity check for low memory
          terminal_printf("[MB Early] Invalid MB info address 0x%x\n", mb_info_phys_addr);
         return NULL;
     }
     volatile uint32_t* mb_info_ptr = (volatile uint32_t*)mb_info_phys_addr;
     uint32_t total_size = mb_info_ptr[0]; // First field is total size
     // uint32_t reserved   = mb_info_ptr[1]; // Second field is reserved
 
     // Sanity check size
     if (total_size < 8 || total_size > 16 * 1024) { // Header is 8 bytes, max reasonable size e.g. 16KB
         terminal_printf("[MB Early] Invalid MB total size %u\n", total_size);
         return NULL;
     }
 
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8); // Tags start after size and reserved fields
     uintptr_t info_end        = mb_info_phys_addr + total_size;
 
     // Iterate through tags
     while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
         uintptr_t current_tag_addr = (uintptr_t)tag;
         // Check tag bounds
         if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || // Ensure basic tag header fits
             tag->size < 8 ||                                              // Minimum tag size
             current_tag_addr + tag->size > info_end) {                    // Ensure full tag fits within total_size
              terminal_printf("[MB Early] Invalid tag found at 0x%x (type %u, size %u)\n", current_tag_addr, tag->type, tag->size);
             return NULL; // Invalid tag structure
         }
 
         if (tag->type == type) {
             return tag; // Found the tag
         }
 
         // Move to the next tag (align size to 8 bytes)
         uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
         if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) { // Check for loop or overflow
              terminal_printf("[MB Early] Invalid next tag address 0x%x calculated from tag at 0x%x\n", next_tag_addr, current_tag_addr);
              break;
         }
         tag = (struct multiboot_tag *)next_tag_addr;
     }
 
     // terminal_printf("[MB Early] Tag type %u not found.\n", type);
     return NULL; // Tag not found
 }
 
 // Marked static as it's an internal early boot helper
 static uintptr_t paging_alloc_early_frame_physical(void) {
     early_allocator_used = true; // Mark that we are using it now
     if (g_multiboot_info_phys_addr_global == 0) {
         terminal_write("[EARLY ALLOC ERROR] Multiboot info address is zero!\n");
         PAGING_PANIC("Early alloc attempted before Multiboot info set!");
     }
 
     // terminal_printf("[EARLY ALLOC] Finding memory map using MB info at PhysAddr 0x%x\n", g_multiboot_info_phys_addr_global);
 
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
                 // Access directly via physical address (assumes identity mapped or accessible)
                 memset((void*)current_frame_addr, 0, PAGE_SIZE);
 
                 // terminal_printf("[EARLY ALLOC] Allocated frame: Phys=0x%x\n", current_frame_addr);
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
              // This case should ideally not happen if logic is correct.
              // It means early allocation was requested *after* paging_finalize_and_activate
              // marked the early allocator as finished.
             PAGING_PANIC("Attempted early frame allocation after early stage finished!");
         }
     } else {
         // Use normal frame allocator (e.g., buddy allocator via frame.c)
         uintptr_t frame = frame_alloc(); // Assumes frame_alloc() is ready
         if (frame == 0) {
             PAGING_PANIC("frame_alloc() failed during normal allocation!");
         }
         // Ensure frame is zeroed by frame_alloc or zero it here if necessary
         // Assuming frame_alloc returns zeroed frames based on common practice
         // If not: memset((void*)PHYS_TO_VIRT(frame), 0, PAGE_SIZE); // Requires PHYS_TO_VIRT
         return frame;
     }
 }
 
 // Marked static as it's an internal helper
 static uint32_t* allocate_page_table_phys(bool use_early_allocator) {
     uintptr_t pt_phys = paging_alloc_frame(use_early_allocator);
     if (!pt_phys) { // paging_alloc_frame already PANICs on failure, but check anyway
         terminal_printf("[Paging] Failed to allocate frame for Page Table (early=%d).\n",
                         use_early_allocator);
         return NULL; // Or PANIC
     }
     // Frame should already be zeroed by paging_alloc_early_frame_physical or frame_alloc
     return (uint32_t*)pt_phys;
 }
 
 // --- CPU Feature Detection ---
 // Made non-static: Modifies global state and enables CR4 bit, might be useful externally.
 bool check_and_enable_pse(void) {
     uint32_t eax, ebx, ecx, edx;
     cpuid(1, &eax, &ebx, &ecx, &edx); // Basic CPUID info
 
     if (edx & CPUID_FEAT_EDX_PSE) { // Check PSE bit (bit 3) in EDX
         terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
         enable_cr4_pse(); // Set CR4.PSE bit (bit 4)
         // Verify CR4.PSE was set
         if (read_cr4() & CR4_PSE) {
             terminal_write("[Paging] CR4.PSE bit enabled.\n");
             g_pse_supported = true;
             return true;
         } else {
             // This would be a major issue, potentially hardware/VM config problem
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
 
 // Marked static as it's an internal helper, only called during init
 static bool check_and_enable_nx(void) {
     uint32_t eax, ebx, ecx, edx;
     cpuid(0x80000000, &eax, &ebx, &ecx, &edx); // Get highest extended function supported
 
     if (eax < 0x80000001) {
         terminal_write("[Paging] CPUID extended function 0x80000001 not supported. Cannot check NX.\n");
         g_nx_supported = false;
         return false;
     }
 
     cpuid(0x80000001, &eax, &ebx, &ecx, &edx); // Get extended feature bits
 
     if (edx & CPUID_FEAT_EDX_NX) { // Check NX bit (bit 20) in EDX
         terminal_write("[Paging] CPU supports NX (Execute Disable) bit.\n");
         // Enable NXE (NX Enable) bit in EFER MSR (Model Specific Register)
         uint64_t efer = rdmsr(MSR_EFER);
         efer |= EFER_NXE; // Set bit 11
         wrmsr(MSR_EFER, efer);
 
         // Verify EFER.NXE was set
         efer = rdmsr(MSR_EFER);
         if (efer & EFER_NXE) {
             terminal_write("[Paging] EFER.NXE bit enabled.\n");
             g_nx_supported = true;
             // Add NX flag to relevant PTE flags constants if needed elsewhere
             // e.g. #define PAGE_NX (1ULL << 63) (though we only use lower 32 bits for entries)
             // The NX bit is the highest bit in the PTE/PDE (bit 63), but accessed via PAE/long mode normally.
             // For 32-bit paging, it's implicitly used when EFER.NXE is set.
             // We add PAGE_NX_BIT to flags where execution should be disallowed.
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
 
 // Made non-static: Called externally during kernel initialization.
 int paging_initialize_directory(uintptr_t* out_initial_pd_phys) {
     terminal_write("[Paging Stage 1] Initializing Page Directory...\n");
 
     // Allocate the frame for the Page Directory using the early allocator
     uintptr_t pd_phys = paging_alloc_early_frame_physical();
     if (!pd_phys) { // Should panic inside allocation function, but double check
         PAGING_PANIC("Failed to allocate initial Page Directory frame!");
     }
     // Frame is guaranteed to be zeroed by the early allocator.
     terminal_printf("  Allocated initial PD at Phys: 0x%x\n", pd_phys);
 
     // Check for PSE support and enable it in CR4. Panic if required and not available.
     if (!check_and_enable_pse()) {
         // Decide if PSE is absolutely required. If yes, panic.
         PAGING_PANIC("PSE support is required but not available/enabled!");
         // If PSE is optional, just continue with g_pse_supported = false
         // terminal_write("Warning: PSE support not enabled, 4MB pages unavailable.\n");
     }
 
     // Check for NX support and enable it in EFER MSR.
     check_and_enable_nx(); // g_nx_supported will be set accordingly.
 
     // Return the physical address of the newly allocated Page Directory
     *out_initial_pd_phys = pd_phys;
 
     terminal_write("[Paging Stage 1] Directory allocated, features checked/enabled.\n");
     return 0; // Success
 }
 
 
 /**
  * @brief Maps a physical memory range to virtual addresses before paging is active.
  * Uses the early frame allocator for page tables. Directly manipulates physical PD/PT.
  * PRECONDITION: page_directory_phys points to a valid, zeroed physical PD frame.
  */
  // Marked static as it's an internal early boot helper
 static int paging_map_physical_early(uintptr_t page_directory_phys,
                                   uintptr_t phys_addr_start,
                                   size_t size,
                                   uint32_t flags,
                                   bool map_to_higher_half)
 {
     if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0 || size == 0) {
         terminal_printf("[Paging Early Map] Invalid PD phys (0x%x) or size (%u).\n",
                         page_directory_phys, (unsigned int)size);
         return -1;
     }
 
     // Align start address down, calculate end address, and align end address up.
     uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_start);
     uintptr_t end_phys;
     if (phys_addr_start > UINTPTR_MAX - size) { // Check overflow for end address calculation
          end_phys = UINTPTR_MAX;
     } else {
         end_phys = phys_addr_start + size;
     }
 
     uintptr_t aligned_end_phys = PAGE_ALIGN_UP(end_phys);
     if (aligned_end_phys < end_phys) { // Check overflow from alignment
          aligned_end_phys = UINTPTR_MAX;
     }
     end_phys = aligned_end_phys; // Use the page-aligned end address
 
     if (end_phys <= current_phys) {
         // terminal_printf("[Paging Early Map] Range [0x%x - 0x%x) resulted in zero size after alignment.\n", phys_addr_start, phys_addr_start+size);
         return 0; // Size is zero or negative after alignment
     }
 
     // Calculate map size for safety check
     size_t map_size = (current_phys < end_phys) ? (end_phys - current_phys) : 0;
 
     // Access PD directly via its physical address (before paging is active)
     // Requires this physical address range to be accessible (e.g., identity mapped by bootloader or <1MB)
     volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;
 
     terminal_printf("  Mapping Phys [0x%x - 0x%x) -> %s (Size: %u KB) with flags 0x%x\n",
                     current_phys, end_phys,
                     map_to_higher_half ? "HigherHalf" : "Identity",
                     (unsigned int)(map_size / 1024),
                     flags);
 
     int page_count = 0;
     int safety_counter = 0;
     const int max_pages_early = (128 * 1024 * 1024) / PAGE_SIZE; // Limit early mapping loops (e.g., 128MB)
 
     while (current_phys < end_phys) {
         // Safety break to prevent runaway loops if end_phys logic is flawed

         
         if (++safety_counter > max_pages_early) {
             terminal_printf("[Paging Early Map] Warning: Safety break after %d pages\n", safety_counter);
             return -1; // Indicate potential error
         }
 
         // Calculate target virtual address
         uintptr_t target_vaddr;
         if (map_to_higher_half) {
             // Check for virtual address overflow when adding kernel base
             if (current_phys > UINTPTR_MAX - KERNEL_SPACE_VIRT_START) {
                 terminal_printf("[Paging Early Map] Virtual address overflow for Phys 0x%x to Higher Half\n", current_phys);
                 return -1;
             }
             target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
         } else {
             // Identity mapping: virt == phys
             target_vaddr = current_phys;
             // Basic check: identity maps should generally stay below kernel space start
             if (target_vaddr >= KERNEL_SPACE_VIRT_START) {
                  terminal_printf("[Paging Early Map] Warning: Identity map target 0x%x overlaps kernel space start 0x%x\n",
                                 target_vaddr, KERNEL_SPACE_VIRT_START);
                  // Allow for now, but could be problematic later.
             }
         }
 
         // Ensure calculated target_vaddr is page aligned (should be if current_phys is)
         if (target_vaddr % PAGE_SIZE != 0) {
             terminal_printf("[Paging Early Map] Internal Error: Target VAddr 0x%x not aligned.\n", target_vaddr);
             return -1;
         }
 
         uint32_t pd_idx = PDE_INDEX(target_vaddr);
         uint32_t pt_idx = PTE_INDEX(target_vaddr);
 
         // Check Page Directory Entry (PDE)
         uint32_t pde = pd_phys_ptr[pd_idx];
         uintptr_t pt_phys_addr;
         volatile uint32_t* pt_phys_ptr;
 
         if (!(pde & PAGE_PRESENT)) {
             // PDE not present: Allocate a new Page Table (PT) frame
             uint32_t* new_pt = allocate_page_table_phys(true); // Use early allocator
             if (!new_pt) {
                 terminal_printf("[Paging Early Map] Failed to allocate PT frame for VAddr 0x%x\n", target_vaddr);
                 return -1; // Allocation failed
             }
             pt_phys_addr = (uintptr_t)new_pt;
             pt_phys_ptr = (volatile uint32_t*)pt_phys_addr; // PT also accessed physically
 
             // Frame is already zeroed by allocator.
             // Set PDE entry: Point to new PT, apply relevant flags (Present, RW, maybe User based on input flags)
             uint32_t pde_flags = PAGE_PRESENT | PAGE_RW; // Default kernel RW
             if (flags & PAGE_USER) {
                 pde_flags |= PAGE_USER;
             }
             pd_phys_ptr[pd_idx] = (pt_phys_addr & ~0xFFF) | pde_flags;
 
              // Debug print for new PT allocation
              // if (page_count < 5 || page_count % 128 == 0) {
              //     terminal_printf("      Allocated PT at Phys 0x%x for VAddr 0x%x (PDE[%d]=0x%x)\n",
              //                     pt_phys_addr, target_vaddr, pd_idx, pd_phys_ptr[pd_idx]);
              // }
 
         } else {
             // PDE is present
             if (pde & PAGE_SIZE_4MB) {
                 // Cannot map a 4K page if a 4M page already covers this virtual address range
                 terminal_printf("[Paging Early Map] Error: Attempt to map 4K page over existing 4M page at VAddr 0x%x (PDE[%d]=0x%x)\n",
                                 target_vaddr, pd_idx, pde);
                 return -1;
             }
             // PDE points to an existing 4K Page Table
             pt_phys_addr = (uintptr_t)(pde & ~0xFFF);
             pt_phys_ptr = (volatile uint32_t*)pt_phys_addr; // Access existing PT physically
 
             // Check if existing PDE needs flags updated (e.g., adding RW or User)
             uint32_t needed_pde_flags = PAGE_PRESENT; // Must be present
             if (flags & PAGE_RW) needed_pde_flags |= PAGE_RW;
             if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;
 
             if ((pde & needed_pde_flags) != needed_pde_flags) {
                  uint32_t old_pde = pde;
                  pd_phys_ptr[pd_idx] |= (needed_pde_flags & (PAGE_RW | PAGE_USER)); // Add missing flags
                  // Debug print for flag promotion
                  // if (page_count < 5 || page_count % 128 == 0) {
                  //     terminal_printf("      Promoted PDE[%d] flags for VAddr 0x%x from 0x%x to 0x%x\n",
                  //                     pd_idx, target_vaddr, old_pde, pd_phys_ptr[pd_idx]);
                  // }
             }
         }
 
         // Now, work with the Page Table Entry (PTE)
         uint32_t pte = pt_phys_ptr[pt_idx];
 
         // Construct the new PTE value
         uint32_t pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD)) | PAGE_PRESENT;
         // Note: PAGE_GLOBAL is generally not used in early mappings. NX bit isn't explicitly set here, relies on EFER.NXE.
         uint32_t new_pte = (current_phys & ~0xFFF) | pte_final_flags;
 
         // Check if PTE is already present
         if (pte & PAGE_PRESENT) {
             // PTE exists. Check if it's the *same* mapping or different.
             if (pte == new_pte) {
                 // Identical mapping already exists. Silently allow, maybe warn.
                 // terminal_printf("[Paging Early Map] Warning: Re-mapping identical V=0x%x -> P=0x%x\n", target_vaddr, current_phys);
             } else {
                 // PTE exists but points elsewhere or has different flags. This is an error.
                 terminal_printf("[Paging Early Map] Error: PTE already present/different for VAddr 0x%x (PTE[%d])\n", target_vaddr, pt_idx);
                 terminal_printf("  Existing PTE = 0x%x (Points to Phys 0x%x)\n", pte, pte & ~0xFFF);
                 terminal_printf("  Attempted PTE = 0x%x (Points to Phys 0x%x)\n", new_pte, new_pte & ~0xFFF);
                 return -1; // Overwriting a different mapping is not allowed here
             }
         }
     
 
         // Set the PTE in the physically accessed Page Table
         pt_phys_ptr[pt_idx] = new_pte;
 
         // Limit debug output to avoid flooding console during large mappings
         // if (page_count < 10 || page_count % 512 == 0) {
         //     terminal_printf("        Set PTE[%d] in PT Phys 0x%x -> Phys 0x%x (Value 0x%x)\n",
         //                    pt_idx, pt_phys_addr, current_phys, new_pte);
         // }
 
         page_count++;
 
         // Advance to the next page frame
         if (current_phys > UINTPTR_MAX - PAGE_SIZE) {
             break; // Physical address overflow
         }
         current_phys += PAGE_SIZE;
     }
 
     terminal_printf("  Mapped %d pages for region.\n", page_count);
     return 0; // Success
 }
 
 // Made non-static: Called externally during kernel initialization.
 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start,
                             uintptr_t kernel_phys_end,
                             uintptr_t heap_phys_start,
                             size_t heap_size)
 {
     terminal_write("[Paging Stage 2] Setting up early memory maps...\n");
 
     // --- Essential Mappings ---
 
     // 1. Identity map the first 1MB (or 4MB) - Crucial for early devices/BIOS access
     //    Map physical 0x0 to virtual 0x0.
     //    Size depends on what bootloader guarantees or what might be needed. 4MB is safer.
     size_t identity_map_size = 4 * 1024 * 1024; // 4MB
     terminal_printf("  Mapping Identity [0x0 - 0x%x)\n", identity_map_size);
     if (paging_map_physical_early(page_directory_phys,
                                   0x0,                    // Physical start 0
                                   identity_map_size,      // Size
                                   PTE_KERNEL_DATA_FLAGS,  // Kernel Read/Write
                                   false) != 0)            // Identity map (map_to_higher_half = false)
     {
         PAGING_PANIC("Failed to set up early identity mapping!");
     }
 
     // 2. Map the Kernel (.text, .rodata, .data, .bss) to the higher half
     //    Map physical [kernel_phys_start, kernel_phys_end) to virtual [KERNEL_SPACE_VIRT_START + start, KERNEL_SPACE_VIRT_START + end)
     uintptr_t kernel_phys_aligned_start = PAGE_ALIGN_DOWN(kernel_phys_start);
     uintptr_t kernel_phys_aligned_end = PAGE_ALIGN_UP(kernel_phys_end);
     size_t kernel_size = kernel_phys_aligned_end - kernel_phys_aligned_start;
     terminal_printf("  Mapping Kernel Phys [0x%x - 0x%x) to Higher Half [0x%x - 0x%x)\n",
                      kernel_phys_aligned_start, kernel_phys_aligned_end,
                      KERNEL_SPACE_VIRT_START + kernel_phys_aligned_start,
                      KERNEL_SPACE_VIRT_START + kernel_phys_aligned_end);
 
     // Map kernel sections with appropriate permissions if possible, otherwise map whole block RWX/RW
     // Simple approach: Map entire kernel block as Kernel RW for now.
     // More complex: Map .text RX, .rodata R, .data/.bss RW using multiple calls.
     if (paging_map_physical_early(page_directory_phys,
                                   kernel_phys_aligned_start, // Physical start of kernel
                                   kernel_size,               // Size of kernel
                                   PTE_KERNEL_DATA_FLAGS,     // Kernel Read/Write (adjust flags for finer control if needed)
                                   true) != 0)                // Higher half map (map_to_higher_half = true)
     {
         PAGING_PANIC("Failed to map kernel to higher half!");
     }
     // Add finer-grained kernel mapping here if desired (Text RX, ROData R, Data RW)
 
     // 3. Map the initial Kernel Heap area to the higher half
     //    Map physical [heap_phys_start, heap_phys_start + heap_size) to virtual [KERNEL_HEAP_VIRT_START, KERNEL_HEAP_VIRT_START + heap_size)
     //    OR map it contiguous with kernel: virtual [KERNEL_SPACE_VIRT_START + heap_phys_start, ...]
     //    Let's assume contiguous for simplicity unless KERNEL_HEAP_VIRT_START is defined differently.
     if (heap_size > 0) {
         uintptr_t heap_phys_aligned_start = PAGE_ALIGN_DOWN(heap_phys_start);
         uintptr_t heap_end = heap_phys_start + heap_size;
          uintptr_t heap_phys_aligned_end = PAGE_ALIGN_UP(heap_end);
          if(heap_phys_aligned_end < heap_end) heap_phys_aligned_end = UINTPTR_MAX; // overflow check
         size_t heap_aligned_size = heap_phys_aligned_end - heap_phys_aligned_start;
 
         terminal_printf("  Mapping Kernel Heap Phys [0x%x - 0x%x) to Higher Half [0x%x - 0x%x)\n",
                      heap_phys_aligned_start, heap_phys_aligned_end,
                      KERNEL_SPACE_VIRT_START + heap_phys_aligned_start,
                      KERNEL_SPACE_VIRT_START + heap_phys_aligned_end);
 
         if (paging_map_physical_early(page_directory_phys,
                                       heap_phys_aligned_start,
                                       heap_aligned_size,
                                       PTE_KERNEL_DATA_FLAGS, // Kernel Read/Write
                                       true) != 0)           // Higher half map
         {
             PAGING_PANIC("Failed to map early kernel heap!");
         }
     }
 
 
     // 4. Map VGA Buffer (if needed for terminal output after paging)
     //    Map physical VGA_PHYS_ADDR to virtual VGA_VIRT_ADDR (usually in higher half)
     terminal_printf("  Mapping VGA Buffer Phys 0x%x to Virt 0x%x\n", VGA_PHYS_ADDR, VGA_VIRT_ADDR);
     if (paging_map_physical_early(page_directory_phys,
                                   VGA_PHYS_ADDR,          // Physical VGA address (e.g., 0xB8000)
                                   PAGE_SIZE,              // Size (typically one page is enough)
                                   PTE_KERNEL_DATA_FLAGS,  // Kernel Read/Write
                                   true) != 0)             // Map to higher half (VGA_VIRT_ADDR) - Requires VGA_VIRT_ADDR >= KERNEL_SPACE_VIRT_START
     {
         // Note: The target virtual address VGA_VIRT_ADDR is derived inside paging_map_physical_early
         // when map_to_higher_half is true. It calculates KERNEL_SPACE_VIRT_START + phys_addr.
         // Ensure VGA_VIRT_ADDR matches KERNEL_SPACE_VIRT_START + VGA_PHYS_ADDR.
         // If VGA_VIRT_ADDR is some *other* high address, map_to_higher_half=true won't work directly.
          // Let's assume VGA_VIRT_ADDR = KERNEL_SPACE_VIRT_START + VGA_PHYS_ADDR for this call.
         PAGING_PANIC("Failed to map VGA buffer!");
     }
 
     // Add any other required early mappings (e.g., ACPI tables, specific hardware)
 
     terminal_write("[Paging Stage 2] Early memory maps configured.\n");
     return 0; // Success
 }
 
 // Debug helper: prints PDE entries in a given range
 // Marked static as it's a debugging helper
 static void debug_print_pd_entries(uint32_t* pd_ptr, uintptr_t vaddr_start, size_t count) {
     // Access method depends: if paging active, use virtual; if inactive, use physical.
     // This function assumes caller provides the correct pointer (physical or virtual).
     terminal_write("--- Debug PD Entries ---\n");
     uint32_t start_idx = PDE_INDEX(vaddr_start);
     uint32_t end_idx = start_idx + count;
     if (end_idx > TABLES_PER_DIR) end_idx = TABLES_PER_DIR;
 
     for (uint32_t idx = start_idx; idx < end_idx; ++idx) {
        uint32_t pde = pd_ptr[idx];
        uintptr_t va = (uintptr_t)idx << PAGING_PDE_SHIFT;
   
        if (pde & PAGE_PRESENT) {
            // *** CORRECTED PRINTF ***
            terminal_printf(" PDE[%4u] (V~0x%08x): 0x%08x (P=%d RW=%d US=%d PS=%d",
                            idx,
                            va,
                            pde,
                            (pde & PAGE_PRESENT) ? 1 : 0,  // Print 1 or 0
                            (pde & PAGE_RW) ? 1 : 0,       // Print 1 or 0
                            (pde & PAGE_USER) ? 1 : 0,      // Print 1 or 0
                            (pde & PAGE_SIZE_4MB) ? 1 : 0); // Print 1 or 0
            // Add prints for other flags (PWT, PCD, A, D, G, NX) if needed, using the same pattern
            // terminal_printf(" PWT=%d PCD=%d A=%d D=%d G=%d NX=%d",
            //                 (pde & PAGE_PWT) ? 1 : 0,
            //                 (pde & PAGE_PCD) ? 1 : 0,
            //                 (pde & PAGE_ACCESSED) ? 1 : 0,
            //                 (pde & PAGE_DIRTY) ? 1 : 0,    // Note: Dirty bit only meaningful in PTE for 4KB pages
            //                 (pde & PAGE_GLOBAL) ? 1 : 0,
            //                 (pde & PAGE_NX_BIT) ? 1 : 0); // Note: NX bit only meaningful in PTE
   
            if (pde & PAGE_SIZE_4MB) {
                 terminal_printf(" Frame=0x%x)\n", pde & PAGING_PDE_ADDR_MASK_4MB);
            } else {
                 terminal_printf(" PT=0x%x)\n", pde & PAGING_PDE_ADDR_MASK_4KB);
            }
        } else {
            // Optionally print non-present entries too
            // terminal_printf(" PDE[%4u] (V~0x%08x): 0x%08x (Not Present)\n", idx, va, pde);
        }
    }
     terminal_write("------------------------\n");
 }
 
 // Finalize and activate paging (including recursive entry)
 // Made non-static: Called externally during kernel initialization.
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes)
 {
     terminal_write("[Paging Stage 3] Finalizing and activating paging...\n");
     if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) {
         PAGING_PANIC("Finalize: Invalid PD physical address!");
     }
 
     // Access the PD via its physical address one last time before activation
     volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;
 
     // --- Set up the Recursive Page Directory Entry ---
     // Map the last PDE (index 1023) to point back to the Page Directory itself.
     // VAddr range covered: 0xFFC00000 - 0xFFFFFFFF
     // RECURSIVE_PDE_INDEX is typically 1023 (0x3FF)
     // RECURSIVE_PDE_VADDR is typically 0xFFC00000 (base address for accessing page tables)
     // RECURSIVE_PD_VADDR is typically 0xFFFFF000 (base address for accessing page directory itself)
     uint32_t recursive_pde_flags = PAGE_PRESENT | PAGE_RW| PAGE_NX_BIT; // Kernel RW access
     pd_phys_ptr[RECURSIVE_PDE_INDEX] = (page_directory_phys & ~0xFFF) | recursive_pde_flags;
 
     terminal_printf("  Set recursive PDE[%d] to point to PD Phys=0x%x (Value=0x%x)\n",
                     RECURSIVE_PDE_INDEX, page_directory_phys, pd_phys_ptr[RECURSIVE_PDE_INDEX]);
 
     // *** DEBUG: Print key PDE entries right before activation ***
     terminal_printf("  PD Entries Before Activation (Accessed via Phys Addr: 0x%x):\n", page_directory_phys);
     debug_print_pd_entries((uint32_t*)pd_phys_ptr, 0x0, 4); // First few entries (identity map)
     debug_print_pd_entries((uint32_t*)pd_phys_ptr, KERNEL_SPACE_VIRT_START, 4); // Kernel map start
     debug_print_pd_entries((uint32_t*)pd_phys_ptr, RECURSIVE_PDE_VADDR, 1); // Recursive entry itself
 
     terminal_write("  Activating Paging (Loading CR3, Setting CR0.PG)...\n");
     paging_activate((uint32_t*)page_directory_phys);
     terminal_write("  Paging HW Activated.\n");
 
     // --- PAGING IS NOW ACTIVE! ---
     // Physical addresses are no longer directly accessible unless mapped.
     // Code execution continues from the higher-half kernel mapping.
     // Memory access now goes through the MMU.
 
     // Calculate the virtual address of the page directory using the recursive mapping
     // PD Virtual Address = Base of recursive PT area + Index of recursive entry * sizeof(entry)
     // RECURSIVE_PD_VADDR = 0xFFFFF000 = 0xFFC00000 + 1023 * 4096 (incorrect, this is PT access)
     // Correct VAddr for PD = RECURSIVE_PDE_VADDR + RECURSIVE_PDE_INDEX * sizeof(uint32_t*) - No this is wrong too.
     // The PD is the 'page table' pointed to by the recursive PDE.
     // So its virtual address is RECURSIVE_PDE_VADDR + (RECURSIVE_PDE_INDEX * PAGE_SIZE)
     // Let's use the common constant: RECURSIVE_PD_VADDR which should be 0xFFFFF000
 
     // Verify the standard recursive addresses:
     // Accessing PT[N]   -> (uint32_t*)(RECURSIVE_PDE_VADDR + N * PAGE_SIZE)
     // Accessing PD entry M -> ((uint32_t*)RECURSIVE_PD_VADDR)[M]
 
     uintptr_t kernel_pd_virt_addr = RECURSIVE_PD_VADDR; // e.g., 0xFFFFF000
 
     terminal_printf("  Setting global pointers: PD Virt=0x%x, PD Phys=0x%x\n",
                     kernel_pd_virt_addr, page_directory_phys);
 
     // Set global pointers now that we can access the PD virtually
     g_kernel_page_directory_phys = page_directory_phys;
     g_kernel_page_directory_virt = (uint32_t*)kernel_pd_virt_addr;
 
     // --- Verification Step ---
     // Read the recursive PDE entry using the *virtual* address of the PD to verify access.
     terminal_printf("  Verifying recursive mapping via virtual access...\n");
     volatile uint32_t recursive_value_read_virt = g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX];
     terminal_printf("  Recursive PDE[%d] read via *Virt* Addr 0x%x gives value: 0x%x\n",
                     RECURSIVE_PDE_INDEX, (uintptr_t)&g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX],
                     recursive_value_read_virt);
 
     // Compare the physical address part of the read value with the known physical address
     uint32_t actual_phys_in_pte = recursive_value_read_virt & ~0xFFF;
     uint32_t expected_phys = page_directory_phys & ~0xFFF;
 
     if (actual_phys_in_pte != expected_phys) {
         terminal_printf("  ERROR: Recursive PDE verification failed!\n");
         terminal_printf("    Expected PD Phys: 0x%x\n", expected_phys);
         terminal_printf("    Physical Addr in PDE read virtually: 0x%x\n", actual_phys_in_pte);
         PAGING_PANIC("Failed to verify recursive mapping post-activation!");
     } else {
         terminal_printf("  Recursive mapping verified successfully.\n");
     }
 
     // *** DEBUG: Print key PDE entries AFTER activation using VIRTUAL address ***
      terminal_printf("  PD Entries After Activation (Accessed via Virt Addr: 0x%x):\n", kernel_pd_virt_addr);
      debug_print_pd_entries(g_kernel_page_directory_virt, 0x0, 4); // First few entries
      debug_print_pd_entries(g_kernel_page_directory_virt, KERNEL_SPACE_VIRT_START, 4); // Kernel start
      debug_print_pd_entries(g_kernel_page_directory_virt, RECURSIVE_PDE_VADDR, 1); // Recursive entry
 
     terminal_write("[Paging Stage 3] Paging enabled and active. Higher half operational.\n");
     early_allocator_used = false; // Mark early allocator as no longer the primary one
     return 0; // Success
 }
 
 
 // Post-Activation Mapping Functions
 
 /**
 * @brief Internal helper to map a single 4KB or 4MB page.
 * Handles PT allocation if needed and updates the target page directory.
 * Can operate on the current PD (using recursive mapping) or another PD (using temporary mapping).
 * INCLUDES DEBUG LOGGING.
 *
 * @param target_page_directory_phys Physical address of the page directory to modify.
 * @param vaddr Virtual address to map.
 * @param paddr Physical address to map to.
 * @param flags Page flags (e.g., PAGE_PRESENT, PAGE_RW, PAGE_USER, PAGE_NX_BIT).
 * Note: Input flags might be masked internally for safety.
 * @param use_large_page True to attempt mapping a 4MB page, false for 4KB.
 * @return 0 on success, negative error code on failure.
 */
static int map_page_internal(uint32_t *target_page_directory_phys, // Physical address of the target PD
                             uintptr_t vaddr,
                             uintptr_t paddr,
                             uint32_t flags, // Input flags from caller
                             bool use_large_page)
{
    // Basic validation
    if (!g_kernel_page_directory_virt || g_kernel_page_directory_phys == 0) {
        PAGING_PANIC("map_page_internal called before paging fully active and globals set!");
        return -1; // Should not be reached
    }
    if (!target_page_directory_phys || ((uintptr_t)target_page_directory_phys % PAGE_SIZE) != 0) {
        terminal_printf("[Map Internal] Invalid target PD phys 0x%x\n", (uintptr_t)target_page_directory_phys);
        return -1;
    }

    // Mask input flags to ensure only valid bits are used
    const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                      PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                      PAGE_SIZE_4MB | PAGE_GLOBAL |
                                      PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3;
    uint32_t effective_flags = flags & VALID_FLAGS_MASK;

    // Check if we are modifying the currently active kernel page directory
    bool is_current_pd = ((uintptr_t)target_page_directory_phys == g_kernel_page_directory_phys);

    // Align addresses according to page size being used
    uintptr_t aligned_vaddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(vaddr) : PAGE_ALIGN_DOWN(vaddr);
    uintptr_t aligned_paddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(paddr) : PAGE_ALIGN_DOWN(paddr);

    // Calculate PD index
    uint32_t pd_idx = PDE_INDEX(aligned_vaddr);

    if (pd_idx == RECURSIVE_PDE_INDEX) {
        terminal_printf("[Map Internal] Error: Attempted to map into recursive Paging range (V=0x%x, PDE %u).\n", vaddr, pd_idx);
        return -1; // Or a specific error code like -EINVAL
    }

    // --- Refined Flag Calculation ---
    // Base flags present in both PDE (for PT) and PTE
    uint32_t base_flags = PAGE_PRESENT;
    if (effective_flags & PAGE_RW)   base_flags |= PAGE_RW;
    if (effective_flags & PAGE_USER) base_flags |= PAGE_USER;
    if (effective_flags & PAGE_PWT)  base_flags |= PAGE_PWT;
    if (effective_flags & PAGE_PCD)  base_flags |= PAGE_PCD;
    
    uint32_t pde_final_flags = 0;
    uint32_t pte_final_flags = 0; // Only used for 4K pages
    
    if (use_large_page) {
        // --- Flags for a 4MB PDE ---
        if (!g_pse_supported) {
            terminal_printf("[Map Internal] Error: Attempted 4MB map, but PSE not supported/enabled.\n");
            return -1;
        }
        pde_final_flags = base_flags | PAGE_SIZE_4MB;
        if (effective_flags & PAGE_ACCESSED) pde_final_flags |= PAGE_ACCESSED;
        if (effective_flags & PAGE_DIRTY)    pde_final_flags |= PAGE_DIRTY;
        if ((effective_flags & PAGE_GLOBAL) && !(effective_flags & PAGE_USER)) {
             pde_final_flags |= PAGE_GLOBAL;
        }
    } else {
        // --- Flags for a 4KB PTE ---
        pte_final_flags = base_flags;
        if (effective_flags & PAGE_ACCESSED) pte_final_flags |= PAGE_ACCESSED;
        if (effective_flags & PAGE_DIRTY)    pte_final_flags |= PAGE_DIRTY;
        if ((effective_flags & PAGE_GLOBAL) && !(effective_flags & PAGE_USER)) {
             pte_final_flags |= PAGE_GLOBAL;
        }
        if ((effective_flags & PAGE_NX_BIT) && g_nx_supported) {
             pte_final_flags |= PAGE_NX_BIT;
        }
        
        // --- Flags for the PDE pointing to the 4KB Page Table ---
        pde_final_flags = base_flags;
        if (pte_final_flags & PAGE_RW) pde_final_flags |= PAGE_RW;
        if (pte_final_flags & PAGE_USER) pde_final_flags |= PAGE_USER;
        if (pte_final_flags & PAGE_PWT) pde_final_flags |= PAGE_PWT;
        if (pte_final_flags & PAGE_PCD) pde_final_flags |= PAGE_PCD;
    }

    // *** START DEBUG LOGGING ***
    //terminal_printf("MAP_INT PRE-CHECK: V=0x%x->P=0x%x UseLarge=%d | InputFlags=0x%x EffFlags=0x%x BaseFlags=0x%x PDEFlags=0x%x PTEFlags=0x%x\n",
                    //aligned_vaddr, aligned_paddr, use_large_page, flags, effective_flags, base_flags, pde_final_flags, pte_final_flags);
    // *** END DEBUG LOGGING ***

    // --- Modify Page Directory / Page Table ---

    if (is_current_pd) {
        // --- Operate on CURRENT Page Directory (use recursive mapping) ---

        if (use_large_page) {
            // --- Map 4MB page in current PD ---
            uint32_t new_pde_val_4mb = (aligned_paddr & PAGING_PDE_ADDR_MASK_4MB) | pde_final_flags;
            uint32_t current_pde = g_kernel_page_directory_virt[pd_idx];

            if (current_pde & PAGE_PRESENT) {
                if (current_pde == new_pde_val_4mb) return 0; // Identical mapping exists
                terminal_printf("[Map Internal] Error: PDE[%d] already present (value 0x%x), cannot map 4MB page at V=0x%x\n",
                                pd_idx, current_pde, aligned_vaddr);
                return -1;
            }
            // *** DEBUG LOGGING ***
            //terminal_printf("MAP_INT DEBUG 4MB: V=0x%x -> P=0x%x | Setting PDE[%d] = 0x%08x\n",
                            //aligned_vaddr, aligned_paddr, pd_idx, new_pde_val_4mb);
            // *** END DEBUG LOGGING ***
            g_kernel_page_directory_virt[pd_idx] = new_pde_val_4mb;
            paging_invalidate_page((void*)aligned_vaddr);
            return 0;

        } else {
            // --- Map 4KB page in current PD ---
            uint32_t pde = g_kernel_page_directory_virt[pd_idx];
            uint32_t pt_idx = PTE_INDEX(aligned_vaddr);
            uint32_t* pt_virt; // Virtual address of the Page Table
            uintptr_t pt_phys_addr = 0; // Physical address of the PT
            bool pt_allocated_here = false;

            if (!(pde & PAGE_PRESENT)) {
                // --- PDE Not Present: Allocate and setup new PT ---
                pt_phys_addr = frame_alloc();
                if (pt_phys_addr == 0) { terminal_printf("[Map Internal] Error: frame_alloc failed for PT.\n"); return -1; }
                pt_allocated_here = true;

                uint32_t pde_value_to_write = (pt_phys_addr & PAGING_ADDR_MASK) | pde_final_flags | PAGE_PRESENT;
                // *** DEBUG LOGGING ***
                terminal_printf("MAP_INT DEBUG NEW_PT: V=0x%x | Setting PDE[%d] = 0x%08x (PT Phys=0x%x)\n",
                                aligned_vaddr, pd_idx, pde_value_to_write, pt_phys_addr);
                // *** END DEBUG LOGGING ***
                g_kernel_page_directory_virt[pd_idx] = pde_value_to_write;
                paging_invalidate_page((void*)aligned_vaddr); // Invalidate range covered by PDE

                pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
                memset(pt_virt, 0, PAGE_SIZE);

            } else if (pde & PAGE_SIZE_4MB) {
                 terminal_printf("[Map Internal] Error: Attempted 4KB map over existing 4MB page at V=0x%x (PDE[%d]=0x%x)\n",
                                aligned_vaddr, pd_idx, pde);
                return -1;
            } else {
                // --- PDE Present and points to a 4KB PT: REUSE IT ---
                pt_phys_addr = pde & PAGING_ADDR_MASK; // Get phys addr for logging
                pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
                uint32_t needed_pde_flags = pde_final_flags;
                if ((pde & needed_pde_flags) != needed_pde_flags) {
                    uint32_t promoted_pde_val = pde | (needed_pde_flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD));
                    // *** DEBUG LOGGING ***
                    terminal_printf("MAP_INT DEBUG PROMOTE_PDE: V=0x%x | Promoting PDE[%d] from 0x%x to 0x%x\n",
                                    aligned_vaddr, pd_idx, pde, promoted_pde_val);
                    // *** END DEBUG LOGGING ***
                    g_kernel_page_directory_virt[pd_idx] = promoted_pde_val;
                    paging_invalidate_page((void*)aligned_vaddr);
                }
            }

            // --- Set the PTE in the (potentially new or existing) PT ---
            uint32_t new_pte_val_4kb = (aligned_paddr & PAGING_ADDR_MASK) | pte_final_flags;

            if (pt_virt[pt_idx] & PAGE_PRESENT) {
                uint32_t existing_pte_val = pt_virt[pt_idx];
                uintptr_t existing_phys = existing_pte_val & PAGING_ADDR_MASK;
                if (existing_phys == (aligned_paddr & PAGING_ADDR_MASK)) {
                    if (existing_pte_val != new_pte_val_4kb) {
                        // *** DEBUG LOGGING ***
                        terminal_printf("MAP_INT DEBUG 4KB_UPDATE: V=0x%x -> P=0x%x | Updating PTE[%d] in PT@0x%x from 0x%08x to 0x%08x\n",
                                        aligned_vaddr, aligned_paddr, pt_idx, pt_phys_addr, existing_pte_val, new_pte_val_4kb);
                        // *** END DEBUG LOGGING ***
                        pt_virt[pt_idx] = new_pte_val_4kb; // Update flags
                        paging_invalidate_page((void*)aligned_vaddr);
                    } // else: identical mapping, do nothing
                    return 0; // Success (already mapped or flags updated)
                } else {
                    terminal_printf("[Map Internal] Error: PTE[%d] already present for V=0x%x but maps to different P=0x%x (tried P=0x%x)\n",
                                        pt_idx, aligned_vaddr, existing_phys, aligned_paddr);
                    if (pt_allocated_here) { put_frame(pt_phys_addr); g_kernel_page_directory_virt[pd_idx] = 0; paging_invalidate_page((void*)aligned_vaddr); }
                    return -1;
                }
            }

            // PTE was not present, set the new PTE
            // *** DEBUG LOGGING ***
            terminal_printf("MAP_INT DEBUG 4KB_SET: V=0x%x -> P=0x%x | Setting PTE[%d] in PT@0x%x = 0x%08x\n",
                            aligned_vaddr, aligned_paddr, pt_idx, pt_phys_addr, new_pte_val_4kb);
            // *** END DEBUG LOGGING ***
            pt_virt[pt_idx] = new_pte_val_4kb;
            paging_invalidate_page((void*)aligned_vaddr);
            return 0; // Success
        }

    } else {
        // --- Operate on NON-CURRENT Page Directory (use temporary mapping) ---
        int ret = -1;
        bool pt_allocated_here = false;
        uint32_t* target_pd_virt_temp = NULL;
        uint32_t* target_pt_virt_temp = NULL;
        uintptr_t pt_phys = 0; // Renamed from pt_phys_addr

        // 1. Map target PD temporarily
        if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)target_page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
            terminal_printf("MAP_INT: Failed temp map DST PD\n");
            return -1;
        }
        target_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
        uint32_t pde = target_pd_virt_temp[pd_idx];

        // 2. Handle 4MB vs 4KB page
        if (use_large_page) {
            uint32_t new_pde_val_4mb = (aligned_paddr & PAGING_PDE_ADDR_MASK_4MB) | pde_final_flags;
            if (pde & PAGE_PRESENT) {
                terminal_printf("MAP_INT: OTHER PD 4MB conflict\n");
                ret = -1;
            } else {
                terminal_printf("MAP_INT OTHER_PD DEBUG 4MB: V=0x%x -> P=0x%x | Setting PDE[%d] = 0x%08x\n",
                                aligned_vaddr, aligned_paddr, pd_idx, new_pde_val_4mb);
                target_pd_virt_temp[pd_idx] = new_pde_val_4mb;
                ret = 0;
            }
            goto cleanup_other_pd;

        } else {
            // Handle 4KB page for other PD
            if (pde & PAGE_PRESENT) {
                if (pde & PAGE_SIZE_4MB) {
                    terminal_printf("MAP_INT: OTHER PD 4KB conflict w 4MB\n");
                    ret = -1;
                    goto cleanup_other_pd;
                }
                pt_phys = pde & PAGING_ADDR_MASK;
                uint32_t needed_pde_flags = pde_final_flags;
                if ((pde & needed_pde_flags) != needed_pde_flags) {
                    uint32_t promoted_pde_val = pde | (needed_pde_flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD));
                    terminal_printf("MAP_INT OTHER_PD DEBUG PROMOTE_PDE: V=0x%x | Promoting PDE[%d] from 0x%x to 0x%x\n",
                                    aligned_vaddr, pd_idx, pde, promoted_pde_val);
                    target_pd_virt_temp[pd_idx] = promoted_pde_val;
                }
            } else {
                // Allocate new PT
                uint32_t* pt_phys_ptr = allocate_page_table_phys(false); // Use normal allocator
                if (!pt_phys_ptr) {
                    terminal_printf("MAP_INT: OTHER PD failed PT alloc\n");
                    ret = -1;
                    goto cleanup_other_pd;
                }
                pt_phys = (uintptr_t)pt_phys_ptr;
                pt_allocated_here = true;
                // Map temp to zero PT
                if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
                    terminal_printf("MAP_INT: OTHER PD failed temp map new PT\n");
                    put_frame(pt_phys);
                    ret = -1;
                    goto cleanup_other_pd;
                }
                memset((void*)TEMP_MAP_ADDR_PT_DST, 0, PAGE_SIZE);
                kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);

                uint32_t pde_value_to_write = (pt_phys & PAGING_ADDR_MASK) | pde_final_flags | PAGE_PRESENT;
                terminal_printf("MAP_INT OTHER_PD DEBUG NEW_PT: V=0x%x | Setting PDE[%d] = 0x%08x (PT Phys=0x%x)\n",
                                aligned_vaddr, pd_idx, pde_value_to_write, pt_phys);
                target_pd_virt_temp[pd_idx] = pde_value_to_write;
            }

            // 3. Map target PT temporarily
            if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
                terminal_printf("MAP_INT: OTHER PD failed temp map existing PT\n");
                if (pt_allocated_here) put_frame(pt_phys);
                ret = -1;
                goto cleanup_other_pd;
            }
            target_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_DST;

            // 4. Check and set PTE
            uint32_t pt_idx = PTE_INDEX(aligned_vaddr);
            uint32_t current_pte = target_pt_virt_temp[pt_idx];
            uint32_t new_pte_val_4kb = (aligned_paddr & PAGING_ADDR_MASK) | pte_final_flags;

            if (current_pte & PAGE_PRESENT) {
                uint32_t existing_phys = current_pte & PAGING_ADDR_MASK;
                if (existing_phys == (aligned_paddr & PAGING_ADDR_MASK)) {
                    if (current_pte != new_pte_val_4kb) {
                        terminal_printf("MAP_INT OTHER_PD DEBUG 4KB_UPDATE: V=0x%x -> P=0x%x | Updating PTE[%d] in PT@0x%x from 0x%08x to 0x%08x\n",
                                        aligned_vaddr, aligned_paddr, pt_idx, pt_phys, current_pte, new_pte_val_4kb);
                        target_pt_virt_temp[pt_idx] = new_pte_val_4kb; // Update flags
                    }
                    ret = 0; // Success
                } else {
                   terminal_printf("MAP_INT: OTHER PD PTE conflict\n");
                   ret = -1;
                   if (pt_allocated_here) {
                       target_pd_virt_temp[pd_idx] = 0; // Clear PDE
                       put_frame(pt_phys);
                   }
                }
            } else {
                terminal_printf("MAP_INT OTHER_PD DEBUG 4KB_SET: V=0x%x -> P=0x%x | Setting PTE[%d] in PT@0x%x = 0x%08x\n",
                                aligned_vaddr, aligned_paddr, pt_idx, pt_phys, new_pte_val_4kb);
                target_pt_virt_temp[pt_idx] = new_pte_val_4kb;
                ret = 0; // Success
            }
            kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); // Unmap temp PT
        } // End 4KB page handling for other PD

    cleanup_other_pd:
        if (target_pd_virt_temp) { kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST); }
        return ret;

    } // End handling non-current PD
} // End of map_page_internal

int paging_map_range(uint32_t *page_directory_phys,
                     uintptr_t virt_start_addr,
                     uintptr_t phys_start_addr,
                     size_t memsz,
                     uint32_t flags) // Input flags from caller
{
    if (!page_directory_phys || memsz == 0) {
        terminal_printf("[Map Range] Invalid arguments: PD=0x%x, size=%u\n",
                       (uintptr_t)page_directory_phys, (unsigned int)memsz);
        return -1;
    }

    // Define a mask for all valid/allowed flag bits
    const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                     PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                     PAGE_SIZE_4MB | PAGE_GLOBAL |
                                     PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3;

    // Mask the input flags to only allow valid bits
    uint32_t masked_flags = flags & VALID_FLAGS_MASK;
    if (flags != masked_flags) {
         terminal_printf("[Map Range] Warning: Input flags 0x%x contained invalid bits. Using masked flags 0x%x.\n", flags, masked_flags);
    }

    // Align addresses
    uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
    uintptr_t p_start = PAGE_ALIGN_DOWN(phys_start_addr);

    // Calculate end address safely
    uintptr_t v_end;
    if (virt_start_addr > UINTPTR_MAX - memsz) {
        v_end = UINTPTR_MAX;
    } else {
        v_end = virt_start_addr + memsz;
    }
    v_end = PAGE_ALIGN_UP(v_end);

    // Safety check for empty range / overflow from alignment
    if (v_end <= v_start) {
        if (memsz > 0 && (PAGE_ALIGN_UP(virt_start_addr + memsz) <= v_start)) {
            v_end = UINTPTR_MAX;
        } else {
            return 0; // Nothing to map
        }
    }

    // Limit maximum mapping size in one call (optional safety)
    size_t total_size = (v_end > v_start) ? (v_end - v_start) : 0;
    const size_t MAX_SINGLE_MAPPING = 256 * 1024 * 1024; // 256MB limit

    if (total_size > MAX_SINGLE_MAPPING) {
        terminal_printf("[Map Range] Warning: Large mapping of %u MB requested. Limiting to %u MB for this call.\n",
                       (unsigned)(total_size / (1024*1024)),
                       (unsigned)(MAX_SINGLE_MAPPING / (1024*1024)));
        v_end = v_start + MAX_SINGLE_MAPPING;
        if (v_end < v_start) v_end = UINTPTR_MAX; // Handle wrap
        v_end = PAGE_ALIGN_UP(v_end);
        if (v_end <= v_start) v_end = UINTPTR_MAX; // Handle wrap from alignment
    }


    terminal_printf("[Map Range] Mapping V=[0x%x-0x%x) to P=[0x%x...) Flags=0x%x (Masked=0x%x)\n",
                    v_start, v_end, p_start, flags, masked_flags);

    // Mapping loop
    uintptr_t current_v = v_start;
    uintptr_t current_p = p_start;
    int mapped_pages = 0;
    int safety_counter = 0;
    const int max_map_loop_iterations = (MAX_SINGLE_MAPPING / PAGE_SIZE) + 10;

    // *** START OF WHILE LOOP ***
    while (current_v < v_end) {
        // Safety counter
        if (++safety_counter > max_map_loop_iterations) {
            terminal_printf("[Map Range] Safety break after %d iterations\n", safety_counter);
            // Return error instead of breaking to avoid reaching incorrect return path
            return -1;
        }

        size_t remaining_v_size = (v_end > current_v) ? (v_end - current_v) : 0;
        if (remaining_v_size == 0) {
            // Should technically not happen if loop condition is correct, but safe break
            break;
        }

        bool possible_large = g_pse_supported &&
                              (current_v % PAGE_SIZE_LARGE == 0) &&
                              (current_p % PAGE_SIZE_LARGE == 0) &&
                              (remaining_v_size >= PAGE_SIZE_LARGE);

        bool use_large = false; // Default to 4KB

        if (possible_large) {
            // Check PDE before deciding to use large page
            uint32_t pd_idx_check = PDE_INDEX(current_v);
            uint32_t existing_pde_check = 0; // Default to 0 (not present)

            // Safely check the PDE only if paging is active and we know the current PD virt addr
            if (g_kernel_page_directory_virt != NULL && (uintptr_t)page_directory_phys == g_kernel_page_directory_phys) {
                 existing_pde_check = g_kernel_page_directory_virt[pd_idx_check];
            } else if (g_kernel_page_directory_virt != NULL) {
                // Cannot safely check non-current PD without temporary mapping here.
                // Assume we cannot use large page if we cannot check.
                 terminal_printf("[Map Range] Warning: Cannot check PDE for non-current PD 0x%x. Forcing 4KB.\n", page_directory_phys);
                existing_pde_check = PAGE_PRESENT; // Set present to force use_large=false
            } // If paging not active, this check shouldn't be reachable via this path anyway

            if (!(existing_pde_check & PAGE_PRESENT)) {
                 use_large = true;
            } else {
                 use_large = false; // Force 4KB if PDE exists
            }
        }

        // Map this page/region using the determined size (4KB or 4MB)
        int result = map_page_internal(page_directory_phys,
                                     current_v,
                                     current_p,
                                     masked_flags, // Use masked flags
                                     use_large);

        if (result != 0) {
            terminal_printf("[Map Range] Failed map_page_internal for V=0x%x P=0x%x Large=%d\n",
                            current_v, current_p, use_large);
            return -1; // Exit on error
        }

        // Advance pointers
        size_t step = use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;

        // Check for overflow before adding step
        if (current_v > UINTPTR_MAX - step) {
            terminal_printf("[Map Range] Virtual address overflow during iteration.\n");
            break; // Stop mapping if overflow
        }
        uintptr_t next_p = current_p + step;
        if (next_p < current_p) { // Check physical overflow
             terminal_printf("[Map Range] Warning: Physical address overflow during iteration.\n");
             break; // Stop mapping if overflow
        }

        current_v += step;
        current_p = next_p;
        mapped_pages++;

    // *** END OF WHILE LOOP ***
    }

    terminal_printf("[Map Range] Completed. Mapped %d pages/blocks for V=[0x%x - 0x%x).\n",
                   mapped_pages, v_start, current_v); // Use current_v for actual end mapped
    return 0; // Success
}
 
 // --- Utility Functions ---
 
 // Made non-static: Public API function.
 int paging_get_physical_address(uint32_t *page_directory_phys, // Target PD physical address
                                 uintptr_t vaddr,
                                 uintptr_t *paddr_out) // Output physical address
 {
     if (!page_directory_phys || !paddr_out) return -1;
     // Assumes paging is active for temporary mapping to work
     if (!g_kernel_page_directory_virt) return -1;
 
     int ret = -1; // Default to failure (not found)
     uint32_t* target_pd_virt_temp = NULL;
     uint32_t* target_pt_virt_temp = NULL;
 
     // 1. Map target PD temporarily
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)page_directory_phys, PTE_KERNEL_READONLY_FLAGS) != 0) {
         terminal_printf("[GetPhys] Failed to temp map target PD 0x%x\n", (uintptr_t)page_directory_phys);
         goto cleanup_get_phys;
     }
     target_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
 
     // 2. Read PDE
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pde = target_pd_virt_temp[pd_idx];
 
     if (!(pde & PAGE_PRESENT)) {
         // terminal_printf("[GetPhys] PDE not present for V=0x%x\n", vaddr);
         goto cleanup_get_phys; // Not mapped at PDE level
     }
 
     // 3. Handle 4MB vs 4KB page
     if (pde & PAGE_SIZE_4MB) {
         // 4MB Page Found
         uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB; // Mask out flags and lower bits
         uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1); // Offset within the 4MB page
         *paddr_out = page_base_phys + page_offset;
         ret = 0; // Success
         // terminal_printf("[GetPhys] 4MB PDE Found: V=0x%x -> P=0x%x\n", vaddr, *paddr_out);
         goto cleanup_get_phys;
     } else {
         // 4KB Page Table - need to check PTE
         uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB; // Get physical address of PT
 
         // 4. Map target PT temporarily
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, pt_phys, PTE_KERNEL_READONLY_FLAGS) != 0) {
             terminal_printf("[GetPhys] Failed to temp map target PT 0x%x for V=0x%x\n", pt_phys, vaddr);
             goto cleanup_get_phys;
         }
         target_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
 
         // 5. Read PTE
         uint32_t pt_idx = PTE_INDEX(vaddr);
         uint32_t pte = target_pt_virt_temp[pt_idx];
 
         if (!(pte & PAGE_PRESENT)) {
             // terminal_printf("[GetPhys] PTE not present for V=0x%x\n", vaddr);
             goto cleanup_get_phys; // Not mapped at PTE level
         }
 
         // 6. Calculate final physical address
         uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK; // Mask out flags
         uintptr_t page_offset = vaddr & (PAGE_SIZE - 1); // Offset within the 4KB page
         *paddr_out = page_base_phys + page_offset;
         ret = 0; // Success
         // terminal_printf("[GetPhys] 4KB PTE Found: V=0x%x -> P=0x%x\n", vaddr, *paddr_out);
     }
 
     cleanup_get_phys:
     // 7. Unmap temporary PT and PD
     if (target_pt_virt_temp) {
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
     }
     if (target_pd_virt_temp) {
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC);
     }
     return ret;
 }
 
 // --- Process Management Support ---
 
 // Made non-static: Public API function called when destroying an address space.
 void paging_free_user_space(uint32_t *page_directory_phys) {
     if (!page_directory_phys || page_directory_phys == (uint32_t*)g_kernel_page_directory_phys) {
          terminal_printf("[FreeUser] Error: Invalid or kernel PD provided (PD Phys: 0x%x)\n", (uintptr_t)page_directory_phys);
         return; // Cannot free kernel space this way, invalid PD
     }
     if (!g_kernel_page_directory_virt) return; // Paging/helpers not ready
 
     terminal_printf("[FreeUser] Freeing user space mappings for PD Phys 0x%x\n", (uintptr_t)page_directory_phys);
 
     uint32_t* target_pd_virt_temp = NULL;
 
     // Map the target PD temporarily for modification
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         terminal_printf("[FreeUser] Error: Failed to temp map target PD 0x%x\n", (uintptr_t)page_directory_phys);
         return;
     }
     target_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
 
     // Iterate through user-space PDE slots (0 to KERNEL_PDE_INDEX - 1)
     // KERNEL_PDE_INDEX is the index of the first PDE dedicated to kernel space (e.g., 768 for 3GB split)
     for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t pde = target_pd_virt_temp[i];
 
         if (pde & PAGE_PRESENT) {
             if (pde & PAGE_SIZE_4MB) {
                 // Found a 4MB user page. Need to decrement ref count for all frames it covers.
                 uintptr_t frame_base = pde & PAGING_PDE_ADDR_MASK_4MB;
                 // Decrement ref count for each 4KB frame within the 4MB page
                 for (size_t f = 0; f < PAGES_PER_TABLE; ++f) { // 1024 frames in 4MB
                      uintptr_t frame_addr = frame_base + f * PAGE_SIZE;
                      if(frame_addr < frame_base) break; // overflow check
                      put_frame(frame_addr);
                  }
                  terminal_printf("  Freed 4MB Page Frames for PDE[%d]\n", i);
 
             } else {
                 // Found a PDE pointing to a user Page Table. Free frames and the PT itself.
                 uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;
                 uint32_t* target_pt_virt_temp = NULL;
 
                 // Map the user PT temporarily
                 if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_READONLY_FLAGS) == 0) {
                     target_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
                     // Iterate through PTEs in this table
                     for (size_t j = 0; j < PAGES_PER_TABLE; ++j) {
                         uint32_t pte = target_pt_virt_temp[j];
                         if (pte & PAGE_PRESENT) {
                             uintptr_t frame_phys = pte & PAGING_PTE_ADDR_MASK;
                             put_frame(frame_phys); // Decrement ref count for the user page frame
                         }
                         // No need to clear PTE here, we free the whole PT below
                     }
                     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); // Unmap the PT
                  } else {
                     terminal_printf("[FreeUser] Warning: Failed to temp map PT 0x%x from PDE[%d] - frames leak!\n", pt_phys, i);
                  }
                  // Free the Page Table frame itself
                  put_frame(pt_phys);
                  // terminal_printf("  Freed Page Table 0x%x and its frames for PDE[%d]\n", pt_phys, i);
             }
             // Clear the PDE entry in the target PD
             target_pd_virt_temp[i] = 0;
         }
     }
 
     // Unmap the target PD
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     // Note: Does not free the PD frame itself, caller must do that.
      terminal_printf("[FreeUser] User space mappings cleared for PD Phys 0x%x\n", (uintptr_t)page_directory_phys);
 }
 
 
 // Made non-static: Public API function used by fork/process creation.
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
    terminal_printf("[CloneDir] Cloning PD 0x%x -> New PD 0x%x\n", (uintptr_t)src_pd_phys_addr, new_pd_phys);

    uint32_t* src_pd_virt_temp = NULL;
    uint32_t* dst_pd_virt_temp = NULL;
    int error_occurred = 0;
    uintptr_t allocated_pt_phys[KERNEL_PDE_INDEX];
    int allocated_pt_count = 0;
    memset(allocated_pt_phys, 0, sizeof(allocated_pt_phys));

    if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)src_pd_phys_addr, PTE_KERNEL_READONLY_FLAGS) != 0) {
        terminal_printf("[CloneDir] Error: Failed to map source PD 0x%x.\n", (uintptr_t)src_pd_phys_addr);
        error_occurred = 1; goto cleanup_clone_err;
    }
    src_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;

    if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, new_pd_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
        terminal_printf("[CloneDir] Error: Failed to map destination PD 0x%x.\n", new_pd_phys);
        error_occurred = 1; goto cleanup_clone_err;
    }
    dst_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_DST;

    for (size_t i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++) {
        dst_pd_virt_temp[i] = g_kernel_page_directory_virt[i];
    }
    dst_pd_virt_temp[RECURSIVE_PDE_INDEX] = (new_pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    for (size_t i = 0; i < KERNEL_PDE_INDEX; i++) {
        uint32_t src_pde = src_pd_virt_temp[i];
        if (!(src_pde & PAGE_PRESENT)) { dst_pd_virt_temp[i] = 0; continue; }

        if (src_pde & PAGE_SIZE_4MB) {
             dst_pd_virt_temp[i] = src_pde;
             uintptr_t frame_base = src_pde & PAGING_PDE_ADDR_MASK_4MB; // Use constant
             for (size_t f = 0; f < PAGES_PER_TABLE; ++f) {
                 uintptr_t frame_addr_to_inc = frame_base + f * PAGE_SIZE;
                 if (frame_addr_to_inc < frame_base) break;
                 get_frame(frame_addr_to_inc);
             }
            continue;
        }

        uintptr_t src_pt_phys = src_pde & PAGING_PDE_ADDR_MASK_4KB; // Use constant
        uintptr_t dst_pt_phys = paging_alloc_frame(false);
        if (!dst_pt_phys) {
            terminal_printf("[CloneDir] Error: Failed to allocate new PT for PDE[%d].\n", i);
            error_occurred = 1; goto cleanup_clone_err;
        }
        // FIX: Check bounds before accessing array
        if ((size_t)allocated_pt_count < ARRAY_SIZE(allocated_pt_phys)) { // Use ARRAY_SIZE macro
            allocated_pt_phys[allocated_pt_count++] = dst_pt_phys;
        } else {
             terminal_printf("[CloneDir] Error: Exceeded allocated_pt_phys array size.\n");
             put_frame(dst_pt_phys);
             error_occurred = 1; goto cleanup_clone_err;
        }

        uint32_t* src_pt_virt_temp = NULL;
        uint32_t* dst_pt_virt_temp = NULL;

        if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, src_pt_phys, PTE_KERNEL_READONLY_FLAGS) != 0) {
            terminal_printf("[CloneDir] Error: Failed to map source PT 0x%x.\n", src_pt_phys);
            error_occurred = 1; goto cleanup_clone_err;
        }
        src_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;

        if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, dst_pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
            terminal_printf("[CloneDir] Error: Failed to map destination PT 0x%x.\n", dst_pt_phys);
            kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
            error_occurred = 1; goto cleanup_clone_err;
        }
        dst_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_DST;

        for (size_t j = 0; j < PAGES_PER_TABLE; j++) {
            uint32_t src_pte = src_pt_virt_temp[j];
            if (src_pte & PAGE_PRESENT) {
                uintptr_t frame_phys = src_pte & PAGING_PTE_ADDR_MASK; // Use constant
                get_frame(frame_phys);
                dst_pt_virt_temp[j] = src_pte;
                // Add COW logic here if needed
            } else {
                dst_pt_virt_temp[j] = 0;
            }
        }

        kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
        kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
        dst_pd_virt_temp[i] = (dst_pt_phys & PAGING_ADDR_MASK) | (src_pde & PAGING_FLAG_MASK);
    }

cleanup_clone_err:
    if (src_pd_virt_temp) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC);
    if (dst_pd_virt_temp) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);

    if (error_occurred) {
        terminal_printf("[CloneDir] Error occurred. Cleaning up allocations...\n");
        for (int k = 0; k < allocated_pt_count; k++) {
            if (allocated_pt_phys[k] != 0) put_frame(allocated_pt_phys[k]);
        }
        if (new_pd_phys != 0) put_frame(new_pd_phys);
        return 0;
    }

    terminal_printf("[CloneDir] Successfully cloned PD to 0x%x\n", new_pd_phys);
    return new_pd_phys;
}
 
 
 // --- Page Fault Handler ---
 
 // Made non-static: Needs to be registered as the interrupt handler for #PF (IRQ 14).
 void page_fault_handler(registers_t *regs) {
     uintptr_t fault_addr;
     asm volatile("mov %%cr2, %0" : "=r"(fault_addr)); // Get faulting address from CR2
     uint32_t error_code = regs->err_code; // Error code pushed by CPU
 
     // Decode error code bits
     bool present      = (error_code & 0x1);  // 0: Non-present page; 1: Access rights violation
     bool write        = (error_code & 0x2);  // 0: Read access; 1: Write access
     bool user         = (error_code & 0x4);  // 0: Supervisor access; 1: User access
     bool reserved_bit = (error_code & 0x8);  // 1: Reserved bit set in page entry
     bool instruction_fetch = (error_code & 0x10); // 1: Fault caused by instruction fetch (requires NX check)
 
     pcb_t* current_process = get_current_process(); // Assumes scheduler provides this
     uint32_t current_pid = current_process ? current_process->pid : (uint32_t)-1; // Get PID or -1 if no process
 
     terminal_printf("\n--- PAGE FAULT (#PF) ---\n");
     terminal_printf(" PID: %d, Addr: 0x%x, ErrCode: 0x%x\n", current_pid, fault_addr, error_code);
     terminal_printf(" Details: %s, %s, %s, %s, %s\n",
                     present ? "Present" : "Not-Present",
                     write ? "Write" : "Read",
                     user ? "User" : "Supervisor",
                     reserved_bit ? "Reserved-Bit-Set" : "Reserved-OK",
                     instruction_fetch ? (g_nx_supported ? "Instruction-Fetch(NX?)" : "Instruction-Fetch") : "Data-Access");
     terminal_printf(" CPU State: EIP=0x%x, CS=0x%x, EFLAGS=0x%x\n", regs->eip, regs->cs, regs->eflags);
     if (user) {
          terminal_printf("            ESP=0x%x, SS=0x%x\n", regs->user_esp, regs->user_ss);
     }
 
 
     // --- Kernel (Supervisor) Fault ---
     if (!user) {
         terminal_printf(" Reason: Fault occurred in Supervisor Mode!\n");
         terminal_printf(" EAX=0x%x EBX=0x%x ECX=0x%x EDX=0x%x\n", regs->eax, regs->ebx, regs->ecx, regs->edx);
         terminal_printf(" ESI=0x%x EDI=0x%x EBP=0x%x ESP=0x%x\n", regs->esi, regs->edi, regs->ebp, regs->esp_dummy);
         // Kernel faults are usually unrecoverable bugs.
         PAGING_PANIC("Irrecoverable Supervisor Page Fault");
     }
 
     // --- User Mode Fault ---
     terminal_printf(" Reason: Fault occurred in User Mode.\n");

     // *** ADD THIS CHECK ***
     if (!current_process || !current_process->mm) {
         terminal_printf("  Error: No current process or mm_struct available for user fault! Addr=0x%x\n", fault_addr);
         PAGING_PANIC("User Page Fault without process context!"); // Halt directly
     }
     mm_struct_t *mm = current_process->mm;
     if (!mm->pgd_phys) {
          terminal_printf("  Error: Current process mm_struct has no page directory!\n");
          goto kill_process;
     }
 
     // Check for specific fatal error conditions first
     if (reserved_bit) {
         terminal_printf("  Error: Reserved bit set in page table entry. Corrupted mapping?\n");
         goto kill_process;
     }
 
     // Check for No-Execute (NX) violation if supported and it was an instruction fetch
     if (g_nx_supported && instruction_fetch) {
         terminal_printf("  Error: Instruction fetch from a No-Execute page (NX violation).\n");
         // Could potentially check VMA flags here too, but NX bit itself is definitive HW signal.
         goto kill_process;
     }
 
     // --- Consult Virtual Memory Areas (VMAs) ---
     vma_struct_t *vma = find_vma(mm, fault_addr); // Find VMA covering the fault address
 
     if (!vma) {
         terminal_printf("  Error: No VMA covers the faulting address 0x%x. Segmentation Fault.\n", fault_addr);
         goto kill_process; // Address is outside any valid allocated region for this process
     }
 
     // VMA found, check permissions against the fault type
     terminal_printf("  VMA Found: [0x%x - 0x%x) Flags: %c%c%c\n",
                     vma->vm_start, vma->vm_end,
                     (vma->vm_flags & VM_READ) ? 'R' : '-',
                     (vma->vm_flags & VM_WRITE) ? 'W' : '-',
                     (vma->vm_flags & VM_EXEC) ? 'X' : '-');
 
     // Check Write Permission
     if (write && !(vma->vm_flags & VM_WRITE)) {
         terminal_printf("  Error: Write attempt to non-writable VMA. Segmentation Fault.\n");
         goto kill_process;
     }
     // Check Read Permission (relevant if not a write fault)
     // Note: Instruction fetch implies a read is also needed.
     if (!write && !(vma->vm_flags & VM_READ)) {
          terminal_printf("  Error: Read attempt (or exec) from non-readable VMA. Segmentation Fault.\n");
          goto kill_process;
     }
      // Check Execute Permission (relevant if instruction fetch, ignore if NX already handled)
      if (!g_nx_supported && instruction_fetch && !(vma->vm_flags & VM_EXEC)) {
          terminal_printf("  Error: Instruction fetch from non-executable VMA. Segmentation Fault.\n");
          goto kill_process;
      }
 
 
     // --- Handle the Fault (Demand Paging / Copy-on-Write) ---
     // If we got here:
     // - It's a user fault.
     // - Address is within a valid VMA.
     // - Basic VMA permissions match the fault type (R/W/X).
     // This implies either:
     //   a) Page Not Present: Need to allocate a frame and map it (Demand Paging).
     //   b) Write to Read-Only Page: Need to handle Copy-on-Write (COW).
 
     // Call a VMA-specific fault handler function (if implemented in mm.c/vma.c)
     // This function encapsulates demand paging and COW logic.
     int handle_result = handle_vma_fault(mm, vma, fault_addr, error_code);
 
     if (handle_result == 0) {
         // Fault was successfully handled (e.g., page allocated and mapped, COW completed)
         terminal_printf("  Fault Handled by VMA Ops. Resuming process.\n");
         terminal_printf("--------------------------\n");
         return; // Return from interrupt, CPU will re-run the faulting instruction
     } else {
         // VMA handler failed (e.g., out of memory, other error)
         terminal_printf("  Error: handle_vma_fault failed with code %d.\n", handle_result);
         goto kill_process;
     }
 
 
 kill_process:
     // Unhandled or fatal fault - terminate the process
     terminal_printf("--- Unhandled User Page Fault ---\n");
     terminal_printf(" Terminating Process PID %u.\n", current_pid);
     terminal_printf("--------------------------\n");
     // Use scheduler function to terminate the current task
     remove_current_task_with_code(0xDEAD000E); // Use a specific exit code for page fault termination
 
     // Should not return from remove_current_task, but if it does, panic.
     PAGING_PANIC("remove_current_task returned after page fault kill!");
 }
 
 
 // --- TLB Flushing ---
 
 // Made non-static: Public API function.
 void tlb_flush_range(void* start_vaddr, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start_vaddr);
     uintptr_t end_addr;
 
     // Calculate end address carefully for overflow
     if ((uintptr_t)start_vaddr > UINTPTR_MAX - size) {
         end_addr = UINTPTR_MAX;
     } else {
         end_addr = (uintptr_t)start_vaddr + size;
     }
     end_addr = PAGE_ALIGN_UP(end_addr);
     if (end_addr < addr) { // Handle overflow from alignment
         end_addr = UINTPTR_MAX;
     }
 
     // Invalidate page by page
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr);
         if (addr > UINTPTR_MAX - PAGE_SIZE) { // Check before adding to prevent overflow
             break;
         }
         addr += PAGE_SIZE;
     }
 }
 
 // Invalidate a single page containing the given virtual address
 // Implemented in assembly (paging_asm.s) as 'invlpg [vaddr]'
 // extern void paging_invalidate_page(void *vaddr);
 
 
 // --- Global PD Pointer Setup ---
 // Made non-static: Public API function potentially called during init.
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
     if (!pd_virt || !pd_phys) {
          terminal_printf("[PagingSet] Error: Invalid null pointers provided.\n");
          return;
     }
     terminal_printf("[PagingSet] Setting Kernel PD Globals: Virt=0x%x Phys=0x%x\n", (uintptr_t)pd_virt, pd_phys);
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }

 int paging_map_single_4k(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    // Basic alignment checks
    if ((vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) {
         terminal_printf("[MapSingle4k] Error: Unaligned addresses V=0x%x P=0x%x\n", vaddr, paddr);
         return -1;
    }
    // Call the internal helper, explicitly setting use_large_page to false
    return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
}

static bool is_page_table_empty(uint32_t *pt_virt) {
    if (!pt_virt) return true; // Or handle as error? Assume null means empty/invalid.
    for (size_t i = 0; i < PAGES_PER_TABLE; ++i) {
        if (pt_virt[i] != 0) { // Check if any entry is non-zero (could be present or just flags)
            return false;
        }
    }
    return true;
}
/**
 * @brief Unmaps a range of virtual addresses.
 * Frees associated physical frames using the Frame Allocator (`put_frame`).
 * Frees page table frames using the Frame Allocator if they become empty.
 * @param page_directory_phys Physical address of the target page directory.
 * @param virt_start_addr Start virtual address of the range to unmap.
 * @param memsz Size of the range to unmap.
 * @return 0 on success, negative error code on failure.
 */
 int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz) {
    if (!page_directory_phys || memsz == 0) {
        terminal_printf("[Unmap Range] Invalid arguments: PD=0x%x, size=%u\n", (uintptr_t)page_directory_phys, (unsigned int)memsz);
        return -1;
    }
     if (!g_kernel_page_directory_virt) {
          terminal_printf("[Unmap Range] Paging not fully active or kernel PD virt invalid.\n");
          return -1; // Cannot use helpers without paging active
     }

    // Align start address down, calculate end address, and align end address up.
    uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
    uintptr_t v_end;
    if (virt_start_addr > UINTPTR_MAX - memsz) { // Check overflow before adding size
        v_end = UINTPTR_MAX;
    } else {
        v_end = virt_start_addr + memsz;
    }
    v_end = PAGE_ALIGN_UP(v_end);
    if (v_end < (virt_start_addr + memsz)) { // Check overflow from alignment
        v_end = UINTPTR_MAX;
    }

    // Check for zero size after alignment
    if (v_end <= v_start) {
        return 0; // Nothing to unmap
    }

    terminal_printf("[Unmap Range] Request: V=[0x%x - 0x%x) in PD Phys 0x%x\n",
                    v_start, v_end, (uintptr_t)page_directory_phys);

    bool is_current_pd = ((uintptr_t)page_directory_phys == g_kernel_page_directory_phys);
    uint32_t* target_pd_virt = NULL; // Virtual address of target PD (direct or temp)
    int ret = 0; // Assume success unless error occurs

    // --- Determine PD Access Method ---
    if (is_current_pd) {
        target_pd_virt = g_kernel_page_directory_virt;
    } else {
        // Map target PD temporarily (writable)
        if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
            terminal_printf("[Unmap Range] Error: Failed to temp map target PD 0x%x\n", (uintptr_t)page_directory_phys);
            return -1;
        }
        target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
    }

    // --- Iterate through pages in the range ---
    for (uintptr_t current_v = v_start; current_v < v_end; current_v += PAGE_SIZE) {

        uint32_t pd_idx = PDE_INDEX(current_v);
        uint32_t pde = target_pd_virt[pd_idx];

        // --- Check PDE ---
        if (!(pde & PAGE_PRESENT)) {
            // PDE not present, this part of the range is already unmapped (at PDE level)
            // Advance current_v to the start of the next PDE range if beneficial,
            // otherwise just continuing the loop is fine.
            // current_v = (current_v & ~((1 << PAGING_PDE_SHIFT) - 1)) + (1 << PAGING_PDE_SHIFT) - PAGE_SIZE; // Align up to next PDE start, subtract PAGE_SIZE for loop increment
            continue;
        }

        if (pde & PAGE_SIZE_4MB) {
            // Handle 4MB page unmapping.
            // Policy: Only unmap if the *entire* 4MB range [aligned_v, aligned_v + 4MB)
            // is contained within the requested [v_start, v_end) range. Otherwise skip/warn.
            uintptr_t page_4mb_start = PAGE_LARGE_ALIGN_DOWN(current_v);
            uintptr_t page_4mb_end = page_4mb_start + PAGE_SIZE_LARGE;

            if (v_start <= page_4mb_start && v_end >= page_4mb_end) {
                // The requested range fully covers this 4MB page, so unmap it.
                terminal_printf("  Unmapping 4MB page at V=0x%x (PDE[%d])\n", page_4mb_start, pd_idx);
                uintptr_t frame_base = pde & PAGING_PDE_ADDR_MASK_4MB;

                // Clear the PDE
                target_pd_virt[pd_idx] = 0;

                // Invalidate TLB *only if* operating on the current PD
                if (is_current_pd) {
                    // Invalidate the entire 4MB range - multiple invlpg might be needed,
                    // or rely on CR3 reload if that's acceptable. Flushing just the start might not be enough.
                    // Safest is often a full CR3 reload after major changes, but invlpg loop is targeted.
                    for(uintptr_t inv_addr = page_4mb_start; inv_addr < page_4mb_end; inv_addr += PAGE_SIZE) {
                         paging_invalidate_page((void*)inv_addr);
                         if(inv_addr > UINTPTR_MAX - PAGE_SIZE) break; // Prevent overflow
                    }
                }

                // Free all 1024 frames backing the 4MB page
                for (size_t f = 0; f < PAGES_PER_TABLE; ++f) {
                    uintptr_t frame_addr = frame_base + f * PAGE_SIZE;
                    if (frame_addr < frame_base) break; // Overflow check
                    put_frame(frame_addr);
                }
                // Advance loop counter past this 4MB page
                 current_v = page_4mb_end - PAGE_SIZE; // Set current_v so next iteration starts after this 4MB page
            } else {
                // Requested range only partially overlaps a 4MB page. Don't touch it.
                terminal_printf("  Warning: Skipping unmap for V=0x%x as it's within a partially covered 4MB page (PDE[%d]).\n", current_v, pd_idx);
                 // Advance loop counter past this 4MB page
                 current_v = page_4mb_end - PAGE_SIZE;
            }
            continue; // Move to next iteration
        }

        // --- Handle 4KB Page Table ---
        uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;
        uint32_t* pt_virt = NULL; // Virtual address of the target PT

        // --- Access PT ---
        if (is_current_pd) {
            // Access via recursive mapping
            pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
        } else {
            // Map target PT temporarily (writable)
            if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
                terminal_printf("[Unmap Range] Error: Failed to temp map target PT 0x%x for V=0x%x\n", pt_phys, current_v);
                ret = -1; // Mark error
                // Should we continue or abort? Abort is safer.
                goto cleanup_unmap;
            }
            pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
        }

        // --- Access PTE ---
        uint32_t pt_idx = PTE_INDEX(current_v);
        uint32_t pte = pt_virt[pt_idx];

        // --- Check and Unmap PTE ---
        if (pte & PAGE_PRESENT) {
            uintptr_t frame_phys = pte & PAGING_PTE_ADDR_MASK;

            // Clear the PTE
            pt_virt[pt_idx] = 0;

            // Invalidate TLB for this page *only if* operating on the current PD
            if (is_current_pd) {
                paging_invalidate_page((void*)current_v);
            }

            // Free the physical frame
            put_frame(frame_phys);

            // --- Check if Page Table is now Empty ---
            if (is_page_table_empty(pt_virt)) {
                 terminal_printf("  PT at Phys 0x%x (PDE[%d]) became empty. Freeing PT frame.\n", pt_phys, pd_idx);

                 // Clear the PDE pointing to this now-empty PT
                 target_pd_virt[pd_idx] = 0;

                 // Invalidate TLB for this VAddr again (or range covered by PDE) if current PD
                 if (is_current_pd) {
                     paging_invalidate_page((void*)current_v); // Invalidate again after PDE change
                 }

                 // Free the frame that held the Page Table itself
                 put_frame(pt_phys);

                 // Optimization: Since the PT is gone, we can potentially advance current_v
                 // to the start of the next PDE's range. However, the simple loop
                 // increment will also work correctly. Let's keep it simple.
            }
        } // End if PTE present

        // --- Cleanup PT Temp Map ---
        if (!is_current_pd) {
            kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
        }

        // Check for overflow before loop increment (unlikely here, but good practice)
        if (current_v > UINTPTR_MAX - PAGE_SIZE) {
            break;
        }

    } // End for loop iterating through pages

cleanup_unmap:
    // --- Cleanup PD Temp Map ---
    if (!is_current_pd) {
        kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
    }

    terminal_printf("[Unmap Range] Completed for V=[0x%x - 0x%x).\n", v_start, v_end);
    return ret;
}

void copy_kernel_pde_entries(uint32_t *new_pd_virt)
{
    if (!g_kernel_page_directory_virt) {
        terminal_write("[Process] Error: copy_kernel_pde_entries called when kernel PD is NULL!\n");
        // Or PAGING_PANIC("copy_kernel_pde_entries called with NULL kernel PD virtual address");
        return;
    }
    if (!new_pd_virt) {
         terminal_write("[Process] Error: copy_kernel_pde_entries called with NULL destination PD pointer.\n");
         // Or PAGING_PANIC("copy_kernel_pde_entries called with NULL destination PD pointer");
        return;
    }

    // Iterate from the start of kernel space up to (but not including) the recursive entry
    for (size_t i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++) {
        // Copy entry directly if present, ensuring USER flag is cleared
        if (g_kernel_page_directory_virt[i] & PAGE_PRESENT) {
            // Copy kernel PDE, ensuring USER flag is always cleared for kernel mappings
            new_pd_virt[i] = g_kernel_page_directory_virt[i] & ~PAGE_USER;
        } else {
            // If not present in source, ensure it's not present in destination
            new_pd_virt[i] = 0;
        }
    }

    // Explicitly handle the PDE entry just *after* the recursive slot (if any needed),
    // skipping the recursive slot itself (index 1023).
    // The recursive entry for the *new* PD should be set by the caller (e.g., paging_clone_directory or process creation).
    // If TABLES_PER_DIR is > 1024 (unlikely for 32-bit), this loop needs adjustment.
    // Assuming TABLES_PER_DIR is 1024, the loop correctly stops before index 1023.
    // We explicitly zero out the recursive entry in the destination PD to avoid accidental copying.
    new_pd_virt[RECURSIVE_PDE_INDEX] = 0;
}

void* paging_temp_map(uintptr_t phys_addr) {
    // Check if address is page aligned (optional sanity check)
    if (phys_addr % PAGE_SIZE != 0) {
        terminal_printf("[Paging Temp Map] Error: Physical address 0x%x is not page-aligned.\n", phys_addr);
        return NULL;
    }

    // Map into the kernel's page directory at the predefined temporary address
    if (paging_map_single_4k((uint32_t*)g_kernel_page_directory_phys,
                              (uintptr_t)TEMP_MAP_ADDR_PF,
                              phys_addr,
                              PTE_KERNEL_DATA_FLAGS) != 0) // Kernel RW, NX
    {
        terminal_printf("[Paging Temp Map] Error: Failed to map paddr 0x%x to temp vaddr 0x%x.\n", phys_addr, (uintptr_t)TEMP_MAP_ADDR_PF);
        return NULL;
    }

    // Return the predefined temporary virtual address
    return (void*)TEMP_MAP_ADDR_PF;
}

/**
 * @brief Unmaps a temporary kernel mapping created by paging_temp_map.
 * Assumes the mapping was done at the standard temporary virtual address.
 *
 * @param temp_vaddr The virtual address returned by paging_temp_map (should be TEMP_MAP_ADDR_PF).
 */
void paging_temp_unmap(void* temp_vaddr) {
    // Basic check: ensure the address matches the expected temporary address
    if (temp_vaddr != (void*)TEMP_MAP_ADDR_PF) {
        terminal_printf("[Paging Temp Unmap] Warning: Attempting to unmap unexpected address 0x%p (expected 0x%x).\n", temp_vaddr, (uintptr_t)TEMP_MAP_ADDR_PF);
        // Still attempt to unmap it, but log warning.
    }

    // Unmap the single page at the temporary virtual address
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys,
                        (uintptr_t)temp_vaddr,
                        PAGE_SIZE);

    // Invalidate the TLB for the unmapped page
    paging_invalidate_page(temp_vaddr);
}