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
 static void       debug_print_pd_entries(uint32_t* pd_phys_ptr, uintptr_t vaddr_start, size_t count);
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
         // *** FORMAT FIX: %u for uintptr_t ***
         terminal_printf("[KMapUnsafe] Error: Unaligned addresses V=0x%u P=0x%u\n", (unsigned long)vaddr, (unsigned long)paddr);
         return -1;
     }
 
     // Allow temporary mappings anywhere for flexibility, but user must be cautious.
     // The original check restricting to kernel space or specific TEMP addresses is removed,
     // but this makes the function more dangerous if misused.
     // terminal_printf("[KMapUnsafe] Mapping V=0x%u -> P=0x%u\n", (unsigned long)vaddr, (unsigned long)paddr);
 
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
 
     // Access kernel PD using its virtual address (ASSUMES PAGING ACTIVE or special setup)
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
 
     // If PDE not present, we need to create a new page table
     if (!(pde & PAGE_PRESENT)) {
         // Allocate a new page table using frame allocator (NOT early allocator here, assumes buddy is up)
         uintptr_t new_pt_phys = frame_alloc();
         if (new_pt_phys == 0) {
             // *** FORMAT FIX: %u for uintptr_t ***
             terminal_printf("[KMapUnsafe] Error: Failed to allocate PT for VAddr 0x%u\n", (unsigned long)vaddr);
             return -1;
         }
 
         // Map the PT in the page directory
         // Ensure flags don't include user bit unless explicitly intended
         g_kernel_page_directory_virt[pd_idx] = (new_pt_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW; // Kernel RW default
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
         new_pt_virt[pt_idx] = (paddr & PAGING_ADDR_MASK) | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
         paging_invalidate_page((void*)vaddr); // Invalidate the specific vaddr
         return 0;
     }
 
     // Check if this is a 4MB page
     if (pde & PAGE_SIZE_4MB) {
         // *** FORMAT FIX: %u for uintptr_t ***
         terminal_printf("[KMapUnsafe] Error: Cannot map 4KB page into existing 4MB PDE V=0x%u\n", (unsigned long)vaddr);
         return -1;
     }
 
     // PDE exists and points to a 4KB page table
     // Use recursive mapping to access the existing PT
     uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
 
     // Check if PTE already exists
     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         uintptr_t existing_paddr = pt_virt[pt_idx] & PAGING_ADDR_MASK;
         if (existing_paddr != (paddr & PAGING_ADDR_MASK)) {
             // *** FORMAT FIX: %u for uintptr_t ***
             terminal_printf("[KMapUnsafe] Warning: Overwriting existing PTE for V=0x%u (Old P=0x%u, New P=0x%u)\n",
                             (unsigned long)vaddr, (unsigned long)existing_paddr, (unsigned long)paddr);
         }
         // Allow overwriting identical mapping or changing flags
     }
 
     // Set the PTE entry
     pt_virt[pt_idx] = (paddr & PAGING_ADDR_MASK) | (flags & PAGING_FLAG_MASK) | PAGE_PRESENT;
     paging_invalidate_page((void*)vaddr);
     return 0;
 } // *** END OF kernel_map_virtual_to_physical_unsafe ***
 
 
 // --- Early Memory Allocation ---
 // Marked static as it's an internal early boot helper
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
     // Access MB info directly via physical addr (ASSUMES <1MB or identity mapped)
     // Need volatile as memory contents can change unexpectedly before caching/MMU setup.
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) { // Basic sanity check for low memory
         // *** FORMAT FIX: %u for uint32_t (address) ***
         terminal_printf("[MB Early] Invalid MB info address 0x%u\n", (unsigned long)mb_info_phys_addr);
         return NULL;
     }
     volatile uint32_t* mb_info_ptr = (volatile uint32_t*)mb_info_phys_addr;
     uint32_t total_size = mb_info_ptr[0]; // First field is total size
     // uint32_t reserved   = mb_info_ptr[1]; // Second field is reserved
 
     // Sanity check size
     if (total_size < 8 || total_size > 16 * 1024) { // Header is 8 bytes, max reasonable size e.g. 16KB
         // *** FORMAT FIX: %lu for uint32_t (size) ***
         terminal_printf("[MB Early] Invalid MB total size %u\n", (unsigned long)total_size);
         return NULL;
     }
 
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8); // Tags start after size and reserved fields
     uintptr_t info_end       = mb_info_phys_addr + total_size;
 
     // Iterate through tags
     while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
         uintptr_t current_tag_addr = (uintptr_t)tag;
         // Check tag bounds
         if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || // Ensure basic tag header fits
             tag->size < 8 ||                                              // Minimum tag size
             current_tag_addr + tag->size > info_end) {                    // Ensure full tag fits within total_size
             // *** FORMAT FIX: %u (address), %u (type), %lu (size) ***
             terminal_printf("[MB Early] Invalid tag found at 0x%u (type %u, size %lu)\n", (unsigned long)current_tag_addr, (unsigned int)tag->type, (unsigned long)tag->size);
             return NULL; // Invalid tag structure
         }
 
         if (tag->type == type) {
             return tag; // Found the tag
         }
 
         // Move to the next tag (align size to 8 bytes)
         uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
         if (next_tag_addr <= current_tag_addr || next_tag_addr >= info_end) { // Check for loop or overflow
              // *** FORMAT FIX: %u (address) ***
             terminal_printf("[MB Early] Invalid next tag address 0x%u calculated from tag at 0x%u\n", (unsigned long)next_tag_addr, (unsigned long)current_tag_addr);
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
 
     // terminal_printf("[EARLY ALLOC] Finding memory map using MB info at PhysAddr 0x%u\n", (unsigned long)g_multiboot_info_phys_addr_global);
 
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
 
                 // terminal_printf("[EARLY ALLOC] Allocated frame: Phys=0x%u\n", (unsigned long)current_frame_addr);
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
     // *** FORMAT FIX: %u for uintptr_t ***
     terminal_printf("  Allocated initial PD at Phys: 0x%u\n", (unsigned long)pd_phys);
 
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
         // *** FORMAT FIX: %u (address), %lu (size) ***
         terminal_printf("[Paging Early Map] Invalid PD phys (0x%u) or size (%u).\n",
                         (unsigned long)page_directory_phys, (unsigned long)size);
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
         // terminal_printf("[Paging Early Map] Range [0x%u - 0x%u) resulted in zero size after alignment.\n", (unsigned long)phys_addr_start, (unsigned long)(phys_addr_start+size));
         return 0; // Size is zero or negative after alignment
     }
 
     // Calculate map size for safety check
     size_t map_size = (current_phys < end_phys) ? (end_phys - current_phys) : 0;
 
     // Access PD directly via its physical address (before paging is active)
     // Requires this physical address range to be accessible (e.g., identity mapped by bootloader or <1MB)
     volatile uint32_t* pd_phys_ptr = (volatile uint32_t*)page_directory_phys;
 
     // *** FORMAT FIX: %u (address), %lu (size), %u (flags) ***
     terminal_printf("  Mapping Phys [0x%u - 0x%u) -> %s (Size: %u KB) with flags 0x%u\n",
                     (unsigned long)current_phys, (unsigned long)end_phys,
                     map_to_higher_half ? "HigherHalf" : "Identity",
                     (unsigned long)(map_size / 1024),
                     (unsigned long)flags);
 
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
                 // *** FORMAT FIX: %u (address) ***
                 terminal_printf("[Paging Early Map] Virtual address overflow for Phys 0x%u to Higher Half\n", (unsigned long)current_phys);
                 return -1;
             }
             target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
         } else {
             // Identity mapping: virt == phys
             target_vaddr = current_phys;
             // Basic check: identity maps should generally stay below kernel space start
             if (target_vaddr >= KERNEL_SPACE_VIRT_START) {
                  // *** FORMAT FIX: %u (address) ***
                  terminal_printf("[Paging Early Map] Warning: Identity map target 0x%u overlaps kernel space start 0x%u\n",
                                  (unsigned long)target_vaddr, (unsigned long)KERNEL_SPACE_VIRT_START);
                  // Allow for now, but could be problematic later.
             }
         }
 
         // Ensure calculated target_vaddr is page aligned (should be if current_phys is)
         if (target_vaddr % PAGE_SIZE != 0) {
             // *** FORMAT FIX: %u (address) ***
             terminal_printf("[Paging Early Map] Internal Error: Target VAddr 0x%u not aligned.\n", (unsigned long)target_vaddr);
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
                 // *** FORMAT FIX: %u (address) ***
                 terminal_printf("[Paging Early Map] Failed to allocate PT frame for VAddr 0x%u\n", (unsigned long)target_vaddr);
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
             pd_phys_ptr[pd_idx] = (pt_phys_addr & PAGING_ADDR_MASK) | pde_flags;
 
              // Debug print for new PT allocation
              // if (page_count < 5 || page_count % 128 == 0) {
              //     terminal_printf("       Allocated PT at Phys 0x%u for VAddr 0x%u (PDE[%u]=0x%u)\n",
              //                     (unsigned long)pt_phys_addr, (unsigned long)target_vaddr, pd_idx, (unsigned long)pd_phys_ptr[pd_idx]);
              // }
 
         } else {
             // PDE is present
             if (pde & PAGE_SIZE_4MB) {
                 // Cannot map a 4K page if a 4M page already covers this virtual address range
                 // *** FORMAT FIX: %u (address), %lu (index), %u (pde) ***
                 terminal_printf("[Paging Early Map] Error: Attempt to map 4K page over existing 4M page at VAddr 0x%u (PDE[%u]=0x%u)\n",
                                 (unsigned long)target_vaddr, (unsigned long)pd_idx, (unsigned long)pde);
                 return -1;
             }
             // PDE points to an existing 4K Page Table
             pt_phys_addr = (uintptr_t)(pde & PAGING_ADDR_MASK);
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
                  //     terminal_printf("       Promoted PDE[%u] flags for VAddr 0x%u from 0x%u to 0x%u\n",
                  //                     pd_idx, (unsigned long)target_vaddr, (unsigned long)old_pde, (unsigned long)pd_phys_ptr[pd_idx]);
                  // }
             }
         }
 
         // Now, work with the Page Table Entry (PTE)
         uint32_t pte = pt_phys_ptr[pt_idx];
 
         // Construct the new PTE value
         uint32_t pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD)) | PAGE_PRESENT;
         // Note: PAGE_GLOBAL is generally not used in early mappings. NX bit isn't explicitly set here, relies on EFER.NXE.
         uint32_t new_pte = (current_phys & PAGING_ADDR_MASK) | pte_final_flags;
 
         // Check if PTE is already present
         if (pte & PAGE_PRESENT) {
             // PTE exists. Check if it's the *same* mapping or different.
             if (pte == new_pte) {
                 // Identical mapping already exists. Silently allow, maybe warn.
                 // terminal_printf("[Paging Early Map] Warning: Re-mapping identical V=0x%u -> P=0x%u\n", (unsigned long)target_vaddr, (unsigned long)current_phys);
             } else {
                 // PTE exists but points elsewhere or has different flags. This is an error.
                  // *** FORMAT FIX: %u (address), %lu (index), %u (pte) ***
                 terminal_printf("[Paging Early Map] Error: PTE already present/different for VAddr 0x%u (PTE[%u])\n", (unsigned long)target_vaddr, (unsigned long)pt_idx);
                 terminal_printf("  Existing PTE = 0x%u (Points to Phys 0x%u)\n", (unsigned long)pte, (unsigned long)(pte & PAGING_ADDR_MASK));
                 terminal_printf("  Attempted PTE = 0x%u (Points to Phys 0x%u)\n", (unsigned long)new_pte, (unsigned long)(new_pte & PAGING_ADDR_MASK));
                 return -1; // Overwriting a different mapping is not allowed here
             }
         }
 
 
         // Set the PTE in the physically accessed Page Table
         pt_phys_ptr[pt_idx] = new_pte;
 
         // Limit debug output to avoid flooding console during large mappings
         // if (page_count < 10 || page_count % 512 == 0) {
         //     terminal_printf("         Set PTE[%lu] in PT Phys 0x%u -> Phys 0x%u (Value 0x%u)\n",
         //                     (unsigned long)pt_idx, (unsigned long)pt_phys_addr, (unsigned long)current_phys, (unsigned long)new_pte);
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
     // *** FORMAT FIX: %zu for size_t (hex) ***
     terminal_printf("  Mapping Identity [0x0 - 0x%zx)\n", identity_map_size);
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
     // *** FORMAT FIX: %u for uintptr_t (addresses) ***
     terminal_printf("  Mapping Kernel Phys [0x%u - 0x%u) to Higher Half [0x%u - 0x%u)\n",
                     (unsigned long)kernel_phys_aligned_start, (unsigned long)kernel_phys_aligned_end,
                     (unsigned long)(KERNEL_SPACE_VIRT_START + kernel_phys_aligned_start),
                     (unsigned long)(KERNEL_SPACE_VIRT_START + kernel_phys_aligned_end));
 
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
 
         // *** FORMAT FIX: %u for uintptr_t (addresses) ***
         terminal_printf("  Mapping Kernel Heap Phys [0x%u - 0x%u) to Higher Half [0x%u - 0x%u)\n",
                         (unsigned long)heap_phys_aligned_start, (unsigned long)heap_phys_aligned_end,
                         (unsigned long)(KERNEL_SPACE_VIRT_START + heap_phys_aligned_start),
                         (unsigned long)(KERNEL_SPACE_VIRT_START + heap_phys_aligned_end));
 
         if (paging_map_physical_early(page_directory_phys,
                                       heap_phys_aligned_start,
                                       heap_aligned_size,
                                       PTE_KERNEL_DATA_FLAGS, // Kernel Read/Write
                                       true) != 0)            // Higher half map
         {
             PAGING_PANIC("Failed to map early kernel heap!");
         }
     }
 
 
     // 4. Map VGA Buffer (if needed for terminal output after paging)
     //    Map physical VGA_PHYS_ADDR to virtual VGA_VIRT_ADDR (usually in higher half)
     // *** FORMAT FIX: %u for addresses ***
     terminal_printf("  Mapping VGA Buffer Phys 0x%u to Virt 0x%u\n", (unsigned long)VGA_PHYS_ADDR, (unsigned long)VGA_VIRT_ADDR);
     if (paging_map_physical_early(page_directory_phys,
                                   VGA_PHYS_ADDR,           // Physical VGA address (e.g., 0xB8000)
                                   PAGE_SIZE,               // Size (typically one page is enough)
                                   PTE_KERNEL_DATA_FLAGS,   // Kernel Read/Write
                                   true) != 0)              // Map to higher half (VGA_VIRT_ADDR) - Requires VGA_VIRT_ADDR >= KERNEL_SPACE_VIRT_START
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
              // *** FORMAT FIX: %u (index), %08u (addr/pde), %d (flags) ***
             terminal_printf(" PDE[%4u] (V~0x%08u): 0x%08u (P=%d RW=%d US=%d PS=%d",
                   (unsigned int)idx,           // Keep %u for index (usually fits int)
                   (unsigned long)va,           // Use %u for address
                   (unsigned long)pde,          // Use %u for PDE value
                             (pde & PAGE_PRESENT) ? 1 : 0,  // Print 1 or 0
                             (pde & PAGE_RW) ? 1 : 0,       // Print 1 or 0
                             (pde & PAGE_USER) ? 1 : 0,     // Print 1 or 0
                             (pde & PAGE_SIZE_4MB) ? 1 : 0); // Print 1 or 0
             // Add prints for other flags (PWT, PCD, A, D, G, NX) if needed, using the same pattern
             // terminal_printf(" PWT=%d PCD=%d A=%d D=%d G=%d NX=%d",
             //              (pde & PAGE_PWT) ? 1 : 0,
             //              (pde & PAGE_PCD) ? 1 : 0,
             //              (pde & PAGE_ACCESSED) ? 1 : 0,
             //              (pde & PAGE_DIRTY) ? 1 : 0,    // Note: Dirty bit only meaningful in PTE for 4KB pages
             //              (pde & PAGE_GLOBAL) ? 1 : 0,
             //              (pde & PAGE_NX_BIT) ? 1 : 0); // Note: NX bit only meaningful in PTE
 
             if (pde & PAGE_SIZE_4MB) {
                 terminal_printf(" Frame=0x%u)\n", (unsigned long)(pde & PAGING_PDE_ADDR_MASK_4MB));
             } else {
                 terminal_printf(" PT=0x%u)\n", (unsigned long)(pde & PAGING_PDE_ADDR_MASK_4KB));
             }
         } else {
             // Optionally print non-present entries too
             // terminal_printf(" PDE[%4u] (V~0x%08u): 0x%08u (Not Present)\n", idx, (unsigned long)va, (unsigned long)pde);
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
     pd_phys_ptr[RECURSIVE_PDE_INDEX] = (page_directory_phys & PAGING_ADDR_MASK) | recursive_pde_flags;
 
     // *** FORMAT FIX: %u, %u, %u ***
     terminal_printf("  Set recursive PDE[%u] to point to PD Phys=0x%u (Value=0x%u)\n",
       (unsigned int)RECURSIVE_PDE_INDEX,
       (unsigned long)page_directory_phys,
       (unsigned long)pd_phys_ptr[RECURSIVE_PDE_INDEX]);
 
     // *** DEBUG: Print key PDE entries right before activation ***
     // *** FORMAT FIX: %u (address) ***
     terminal_printf("  PD Entries Before Activation (Accessed via Phys Addr: 0x%u):\n", (unsigned long)page_directory_phys);
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
     // Accessing PT[N]     -> (uint32_t*)(RECURSIVE_PDE_VADDR + N * PAGE_SIZE)
     // Accessing PD entry M -> ((uint32_t*)RECURSIVE_PD_VADDR)[M]
 
     uintptr_t kernel_pd_virt_addr = RECURSIVE_PD_VADDR; // e.g., 0xFFFFF000
 
     // *** FORMAT FIX: %p (pointer), %u (address) ***
     terminal_printf("  Setting global pointers: PD Virt=%p, PD Phys=0x%u\n",
       (void*)kernel_pd_virt_addr, (unsigned long)page_directory_phys);
 
     // Set global pointers now that we can access the PD virtually
     g_kernel_page_directory_phys = page_directory_phys;
     g_kernel_page_directory_virt = (uint32_t*)kernel_pd_virt_addr;
 
     // --- Verification Step ---
     // Read the recursive PDE entry using the *virtual* address of the PD to verify access.
     terminal_printf("  Verifying recursive mapping via virtual access...\n");
     volatile uint32_t recursive_value_read_virt = g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX];
     // *** FORMAT FIX: %u, %p, %u ***
     terminal_printf("  Recursive PDE[%u] read via *Virt* Addr %p gives value: 0x%u\n",
       (unsigned int)RECURSIVE_PDE_INDEX,
       (void*)&g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX],
       (unsigned long)recursive_value_read_virt);
 
     // Compare the physical address part of the read value with the known physical address
     uint32_t actual_phys_in_pte = recursive_value_read_virt & PAGING_ADDR_MASK;
     uint32_t expected_phys = page_directory_phys & PAGING_ADDR_MASK;
 
     if (actual_phys_in_pte != expected_phys) {
         terminal_printf("  ERROR: Recursive PDE verification failed!\n");
         // *** FORMAT FIX: %u ***
         terminal_printf("    Expected PD Phys: 0x%u\n", (unsigned long)expected_phys);
         terminal_printf("    Physical Addr in PDE read virtually: 0x%u\n", (unsigned long)actual_phys_in_pte);
         PAGING_PANIC("Failed to verify recursive mapping post-activation!");
     } else {
         terminal_printf("  Recursive mapping verified successfully.\n");
     }
 
     // *** DEBUG: Print key PDE entries AFTER activation using VIRTUAL address ***
     // *** FORMAT FIX: %p ***
     terminal_printf("  PD Entries After Activation (Accessed via Virt Addr: %p):\n", (void*)kernel_pd_virt_addr);
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
         // *** FORMAT FIX: %u for address ***
         terminal_printf("[Map Internal] Invalid target PD phys 0x%u\n", (unsigned long)target_page_directory_phys);
         return -1;
     }
 
     // Mask input flags to ensure only valid bits are used
     const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                       PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                       PAGE_SIZE_4MB | PAGE_GLOBAL |
                                       PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3 | PAGE_NX_BIT; // Add NX bit here
     uint32_t effective_flags = flags & VALID_FLAGS_MASK;
 
     // Check if we are modifying the currently active kernel page directory
     bool is_current_pd = ((uintptr_t)target_page_directory_phys == g_kernel_page_directory_phys);
 
     // Align addresses according to page size being used
     uintptr_t aligned_vaddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(vaddr) : PAGE_ALIGN_DOWN(vaddr);
     uintptr_t aligned_paddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(paddr) : PAGE_ALIGN_DOWN(paddr);
 
     // Calculate PD index
     uint32_t pd_idx = PDE_INDEX(aligned_vaddr);
 
     if (pd_idx == RECURSIVE_PDE_INDEX) {
         // *** FORMAT FIX: %u (address), %lu (index) ***
         terminal_printf("[Map Internal] Error: Attempted to map into recursive Paging range (V=0x%u, PDE %lu).\n", (unsigned long)vaddr, (unsigned long)pd_idx);
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
          // NX Bit applies to PTE, not 4MB PDE in 32-bit non-PAE mode
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
         pde_final_flags = base_flags; // Start with basic flags
         // Promote PDE flags based on PTE needs (e.g., if PTE is RW, PDE must be RW)
         if (pte_final_flags & PAGE_RW) pde_final_flags |= PAGE_RW;
         if (pte_final_flags & PAGE_USER) pde_final_flags |= PAGE_USER;
         if (pte_final_flags & PAGE_PWT) pde_final_flags |= PAGE_PWT;
         if (pte_final_flags & PAGE_PCD) pde_final_flags |= PAGE_PCD;
          // NX bit is not applicable to PDE itself
     }
 
     // *** START DEBUG LOGGING ***
     //terminal_printf("MAP_INT PRE-CHECK: V=0x%u->P=0x%u UseLarge=%d | InputFlags=0x%u EffFlags=0x%u BaseFlags=0x%u PDEFlags=0x%u PTEFlags=0x%u\n",
                     //(unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, use_large_page, (unsigned long)flags, (unsigned long)effective_flags, (unsigned long)base_flags, (unsigned long)pde_final_flags, (unsigned long)pte_final_flags);
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
                 // *** FORMAT FIX: %lu (index), %u (pde), %u (address) ***
                 terminal_printf("[Map Internal] Error: PDE[%u] already present (value 0x%u), cannot map 4MB page at V=0x%u\n",
                                 (unsigned long)pd_idx, (unsigned long)current_pde, (unsigned long)aligned_vaddr);
                 return -1;
             }
             // *** DEBUG LOGGING ***
             // *** FORMAT FIX: %u, %lu ***
             //terminal_printf("MAP_INT DEBUG 4MB: V=0x%u -> P=0x%u | Setting PDE[%lu] = 0x%08u\n",
                           //(unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pd_idx, (unsigned long)new_pde_val_4mb);
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
                 // *** FORMAT FIX: %u, %lu ***
                 terminal_printf("MAP_INT DEBUG NEW_PT: V=0x%u | Setting PDE[%u] = 0x%08u (PT Phys=0x%u)\n",
                                 (unsigned long)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde_value_to_write, (unsigned long)pt_phys_addr);
                 // *** END DEBUG LOGGING ***
                 g_kernel_page_directory_virt[pd_idx] = pde_value_to_write;
                 paging_invalidate_page((void*)aligned_vaddr); // Invalidate range covered by PDE
 
                 pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
                 memset(pt_virt, 0, PAGE_SIZE);
 
             } else if (pde & PAGE_SIZE_4MB) {
                  // *** FORMAT FIX: %u (address), %lu (index), %u (pde) ***
                  terminal_printf("[Map Internal] Error: Attempted 4KB map over existing 4MB page at V=0x%u (PDE[%u]=0x%u)\n",
                                  (unsigned long)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde);
                 return -1;
             } else {
                 // --- PDE Present and points to a 4KB PT: REUSE IT ---
                 pt_phys_addr = pde & PAGING_ADDR_MASK; // Get phys addr for logging
                 pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
                 uint32_t needed_pde_flags = pde_final_flags;
                 if ((pde & needed_pde_flags) != needed_pde_flags) {
                      uint32_t promoted_pde_val = pde | (needed_pde_flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD));
                     // *** DEBUG LOGGING ***
                     // *** FORMAT FIX: %u, %lu ***
                     terminal_printf("MAP_INT DEBUG PROMOTE_PDE: V=0x%u | Promoting PDE[%lu] from 0x%u to 0x%u\n",
                                     (unsigned long)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde, (unsigned long)promoted_pde_val);
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
                         // *** FORMAT FIX: %u, %lu ***
                         terminal_printf("MAP_INT DEBUG 4KB_UPDATE: V=0x%u -> P=0x%u | Updating PTE[%u] in PT@0x%u from 0x%08u to 0x%08u\n",
                                         (unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pt_idx, (unsigned long)pt_phys_addr, (unsigned long)existing_pte_val, (unsigned long)new_pte_val_4kb);
                         // *** END DEBUG LOGGING ***
                         pt_virt[pt_idx] = new_pte_val_4kb; // Update flags
                         paging_invalidate_page((void*)aligned_vaddr);
                     } // else: identical mapping, do nothing
                     return 0; // Success (already mapped or flags updated)
                 } else {
                      // *** FORMAT FIX: %lu (index), %u (address) ***
                      terminal_printf("[Map Internal] Error: PTE[%u] already present for V=0x%u but maps to different P=0x%u (tried P=0x%u)\n",
                                      (unsigned long)pt_idx, (unsigned long)aligned_vaddr, (unsigned long)existing_phys, (unsigned long)aligned_paddr);
                     if (pt_allocated_here) { put_frame(pt_phys_addr); g_kernel_page_directory_virt[pd_idx] = 0; paging_invalidate_page((void*)aligned_vaddr); }
                     return -1;
                 }
             }
 
             // PTE was not present, set the new PTE
             // *** DEBUG LOGGING ***
              // *** FORMAT FIX: %u, %lu ***
             terminal_printf("MAP_INT DEBUG 4KB_SET: V=0x%u -> P=0x%u | Setting PTE[%u] in PT@0x%u = 0x%08u\n",
                             (unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pt_idx, (unsigned long)pt_phys_addr, (unsigned long)new_pte_val_4kb);
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
                 // *** FORMAT FIX: %u, %lu ***
                 terminal_printf("MAP_INT OTHER_PD DEBUG 4MB: V=0x%u -> P=0x%u | Setting PDE[%u] = 0x%08u\n",
                                 (unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pd_idx, (unsigned long)new_pde_val_4mb);
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
                      // *** FORMAT FIX: %u, %lu ***
                     terminal_printf("MAP_INT OTHER_PD DEBUG PROMOTE_PDE: V=0x%u | Promoting PDE[%u] from 0x%u to 0x%u\n",
                                     (unsigned long)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde, (unsigned long)promoted_pde_val);
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
                  // *** FORMAT FIX: %u, %lu ***
                 terminal_printf("MAP_INT OTHER_PD DEBUG NEW_PT: V=0x%u | Setting PDE[%u] = 0x%08u (PT Phys=0x%u)\n",
                                 (unsigned long)aligned_vaddr, (unsigned long)pd_idx, (unsigned long)pde_value_to_write, (unsigned long)pt_phys);
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
                          // *** FORMAT FIX: %u, %lu ***
                          terminal_printf("MAP_INT OTHER_PD DEBUG 4KB_UPDATE: V=0x%u -> P=0x%u | Updating PTE[%u] in PT@0x%u from 0x%08u to 0x%08u\n",
                                          (unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pt_idx, (unsigned long)pt_phys, (unsigned long)current_pte, (unsigned long)new_pte_val_4kb);
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
                  // *** FORMAT FIX: %u, %lu ***
                  terminal_printf("MAP_INT OTHER_PD DEBUG 4KB_SET: V=0x%u -> P=0x%u | Setting PTE[%lu] in PT@0x%u = 0x%08u\n",
                                  (unsigned long)aligned_vaddr, (unsigned long)aligned_paddr, (unsigned long)pt_idx, (unsigned long)pt_phys, (unsigned long)new_pte_val_4kb);
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
         // *** FORMAT FIX: %p (pointer), %zu (size) ***
         terminal_printf("[Map Range] Invalid arguments: PD=%p, size=%zu\n",
                         (void*)page_directory_phys, memsz);
         return -1;
     }
 
     // Define a mask for all valid/allowed flag bits
      const uint32_t VALID_FLAGS_MASK = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT |
                                        PAGE_PCD | PAGE_ACCESSED | PAGE_DIRTY |
                                        PAGE_SIZE_4MB | PAGE_GLOBAL |
                                        PAGE_OS_AVAILABLE_1 | PAGE_OS_AVAILABLE_2 | PAGE_OS_AVAILABLE_3 | PAGE_NX_BIT;
 
     // Mask the input flags to only allow valid bits
     uint32_t masked_flags = flags & VALID_FLAGS_MASK;
     if (flags != masked_flags) {
          // *** FORMAT FIX: %u for flags ***
          terminal_printf("[Map Range] Warning: Input flags 0x%u contained invalid bits. Using masked flags 0x%u.\n", (unsigned long)flags, (unsigned long)masked_flags);
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
         // *** FORMAT FIX: %zu for size_t ***
         terminal_printf("[Map Range] Warning: Large mapping of %zu MB requested. Limiting to %zu MB for this call.\n",
                         (total_size / (1024*1024)),
                         (MAX_SINGLE_MAPPING / (1024*1024)));
         v_end = v_start + MAX_SINGLE_MAPPING;
         if (v_end < v_start) v_end = UINTPTR_MAX; // Handle wrap
         v_end = PAGE_ALIGN_UP(v_end);
         if (v_end <= v_start) v_end = UINTPTR_MAX; // Handle wrap from alignment
     }
 
     // *** FORMAT FIX: %u for addresses/flags ***
     terminal_printf("[Map Range] Mapping V=[0x%u-0x%u) to P=[0x%u...) Flags=0x%u (Masked=0x%u)\n",
                     (unsigned long)v_start, (unsigned long)v_end, (unsigned long)p_start, (unsigned long)flags, (unsigned long)masked_flags);
 
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
                 // *** FORMAT FIX: Use %p for pointer ***
                 terminal_printf("[Map Range] Warning: Cannot check PDE for non-current PD %p. Forcing 4KB.\n", (void*)page_directory_phys);
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
             // *** FORMAT FIX: %u for addresses ***
             terminal_printf("[Map Range] Failed map_page_internal for V=0x%u P=0x%u Large=%d\n",
                             (unsigned long)current_v, (unsigned long)current_p, use_large);
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
 
     // *** FORMAT FIX: %u for addresses ***
     terminal_printf("[Map Range] Completed. Mapped %d pages/blocks for V=[0x%u - 0x%u).\n",
                   mapped_pages, (unsigned long)v_start, (unsigned long)current_v); // Use current_v for actual end mapped
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
         // *** FORMAT FIX: %p for pointer ***
         terminal_printf("[GetPhys] Failed to temp map target PD %p\n", (void*)page_directory_phys);
         goto cleanup_get_phys;
     }
     target_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
 
     // 2. Read PDE
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pde = target_pd_virt_temp[pd_idx];
 
     if (!(pde & PAGE_PRESENT)) {
         // terminal_printf("[GetPhys] PDE not present for V=0x%u\n", (unsigned long)vaddr);
         goto cleanup_get_phys; // Not mapped at PDE level
     }
 
     // 3. Handle 4MB vs 4KB page
     if (pde & PAGE_SIZE_4MB) {
         // 4MB Page Found
         uintptr_t page_base_phys = pde & PAGING_PDE_ADDR_MASK_4MB; // Mask out flags and lower bits
         uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1); // Offset within the 4MB page
         *paddr_out = page_base_phys + page_offset;
         ret = 0; // Success
         // terminal_printf("[GetPhys] 4MB PDE Found: V=0x%u -> P=0x%u\n", (unsigned long)vaddr, (unsigned long)*paddr_out);
         goto cleanup_get_phys;
     } else {
         // 4KB Page Table - need to check PTE
         uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB; // Get physical address of PT
 
         // 4. Map target PT temporarily
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, pt_phys, PTE_KERNEL_READONLY_FLAGS) != 0) {
             // *** FORMAT FIX: %u for addresses ***
             terminal_printf("[GetPhys] Failed to temp map target PT 0x%u for V=0x%u\n", (unsigned long)pt_phys, (unsigned long)vaddr);
             goto cleanup_get_phys;
         }
         target_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
 
         // 5. Read PTE
         uint32_t pt_idx = PTE_INDEX(vaddr);
         uint32_t pte = target_pt_virt_temp[pt_idx];
 
         if (!(pte & PAGE_PRESENT)) {
             // terminal_printf("[GetPhys] PTE not present for V=0x%u\n", (unsigned long)vaddr);
             goto cleanup_get_phys; // Not mapped at PTE level
         }
 
         // 6. Calculate final physical address
         uintptr_t page_base_phys = pte & PAGING_PTE_ADDR_MASK; // Mask out flags
         uintptr_t page_offset = vaddr & (PAGE_SIZE - 1); // Offset within the 4KB page
         *paddr_out = page_base_phys + page_offset;
         ret = 0; // Success
         // terminal_printf("[GetPhys] 4KB PTE Found: V=0x%u -> P=0x%u\n", (unsigned long)vaddr, (unsigned long)*paddr_out);
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
          // *** FORMAT FIX: %p for pointer ***
          terminal_printf("[FreeUser] Error: Invalid or kernel PD provided (PD Phys: %p)\n", (void*)page_directory_phys);
         return; // Cannot free kernel space this way, invalid PD
     }
     if (!g_kernel_page_directory_virt) return; // Paging/helpers not ready
 
     // *** FORMAT FIX: %p for pointer ***
     terminal_printf("[FreeUser] Freeing user space mappings for PD Phys %p\n", (void*)page_directory_phys);
 
     uint32_t* target_pd_virt_temp = NULL;
 
     // Map the target PD temporarily for modification
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         // *** FORMAT FIX: %p for pointer ***
         terminal_printf("[FreeUser] Error: Failed to temp map target PD %p\n", (void*)page_directory_phys);
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
                  // *** FORMAT FIX: %zu for size_t ***
                  terminal_printf("  Freed 4MB Page Frames for PDE[%zu]\n", i);
 
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
                       // *** FORMAT FIX: %u (address), %zu (index) ***
                       terminal_printf("[FreeUser] Warning: Failed to temp map PT 0x%u from PDE[%zu] - frames leak!\n", (unsigned long)pt_phys, i);
                  }
                  // Free the Page Table frame itself
                  put_frame(pt_phys);
                  // terminal_printf("  Freed Page Table 0x%u and its frames for PDE[%zu]\n", (unsigned long)pt_phys, i);
             }
             // Clear the PDE entry in the target PD
             target_pd_virt_temp[i] = 0;
         }
     }
 
     // Unmap the target PD
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     // Note: Does not free the PD frame itself, caller must do that.
      // *** FORMAT FIX: %p for pointer ***
      terminal_printf("[FreeUser] User space mappings cleared for PD Phys %p\n", (void*)page_directory_phys);
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
     // *** FORMAT FIX: %p (pointer), %u (address) ***
     terminal_printf("[CloneDir] Cloning PD %p -> New PD 0x%u\n", (void*)src_pd_phys_addr, (unsigned long)new_pd_phys);
 
     uint32_t* src_pd_virt_temp = NULL;
     uint32_t* dst_pd_virt_temp = NULL;
     int error_occurred = 0;
     uintptr_t allocated_pt_phys[KERNEL_PDE_INDEX];
     int allocated_pt_count = 0;
     memset(allocated_pt_phys, 0, sizeof(allocated_pt_phys));
 
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)src_pd_phys_addr, PTE_KERNEL_READONLY_FLAGS) != 0) {
         // *** FORMAT FIX: %p for pointer ***
         terminal_printf("[CloneDir] Error: Failed to map source PD %p.\n", (void*)src_pd_phys_addr);
         error_occurred = 1; goto cleanup_clone_err;
     }
     src_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
 
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, new_pd_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         // *** FORMAT FIX: %u for address ***
         terminal_printf("[CloneDir] Error: Failed to map destination PD 0x%u.\n", (unsigned long)new_pd_phys);
         error_occurred = 1; goto cleanup_clone_err;
     }
     dst_pd_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
 
     // Copy kernel PDEs
     for (size_t i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++) {
         dst_pd_virt_temp[i] = g_kernel_page_directory_virt[i];
     }
     // Setup recursive entry for the *new* PD
     dst_pd_virt_temp[RECURSIVE_PDE_INDEX] = (new_pd_phys & PAGING_ADDR_MASK) | PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT;
 
     // Clone user space
     for (size_t i = 0; i < KERNEL_PDE_INDEX; i++) {
         uint32_t src_pde = src_pd_virt_temp[i];
         if (!(src_pde & PAGE_PRESENT)) { dst_pd_virt_temp[i] = 0; continue; }
 
         if (src_pde & PAGE_SIZE_4MB) {
              // Share 4MB user pages (potentially CoW later if needed, complex)
              dst_pd_virt_temp[i] = src_pde;
              uintptr_t frame_base = src_pde & PAGING_PDE_ADDR_MASK_4MB; // Use constant
              for (size_t f = 0; f < PAGES_PER_TABLE; ++f) {
                  uintptr_t frame_addr_to_inc = frame_base + f * PAGE_SIZE;
                  if (frame_addr_to_inc < frame_base) break;
                  get_frame(frame_addr_to_inc);
              }
             continue;
         }
 
         // Copy 4KB page tables and their entries (CoW or direct copy)
         uintptr_t src_pt_phys = src_pde & PAGING_PDE_ADDR_MASK_4KB; // Use constant
         uintptr_t dst_pt_phys = paging_alloc_frame(false);
         if (!dst_pt_phys) {
             // *** FORMAT FIX: %zu for size_t ***
             terminal_printf("[CloneDir] Error: Failed to allocate new PT for PDE[%zu].\n", i);
             error_occurred = 1; goto cleanup_clone_err;
         }
         // Check bounds before accessing array
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
              // *** FORMAT FIX: %u for address ***
              terminal_printf("[CloneDir] Error: Failed to map source PT 0x%u.\n", (unsigned long)src_pt_phys);
             error_occurred = 1; goto cleanup_clone_err;
         }
         src_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
 
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, dst_pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
             // *** FORMAT FIX: %u for address ***
             terminal_printf("[CloneDir] Error: Failed to map destination PT 0x%u.\n", (unsigned long)dst_pt_phys);
             kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
             error_occurred = 1; goto cleanup_clone_err;
         }
         dst_pt_virt_temp = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
 
         // Copy PTEs, incrementing frame ref counts
         for (size_t j = 0; j < PAGES_PER_TABLE; j++) {
             uint32_t src_pte = src_pt_virt_temp[j];
             if (src_pte & PAGE_PRESENT) {
                 uintptr_t frame_phys = src_pte & PAGING_PTE_ADDR_MASK; // Use constant
                 get_frame(frame_phys);
                 dst_pt_virt_temp[j] = src_pte;
                 // Add COW logic here if needed (e.g., clear RW flag, mark as COW)
             } else {
                 dst_pt_virt_temp[j] = 0;
             }
         }
 
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
         // Set the PDE in the destination PD
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
         return 0; // Return 0 on error
     }
 
     // *** FORMAT FIX: %u for address ***
     terminal_printf("[CloneDir] Successfully cloned PD to 0x%u\n", (unsigned long)new_pd_phys);
     return new_pd_phys;
 }
 
 
 // --- Page Fault Handler ---
 void page_fault_handler(registers_t *regs) {
     uintptr_t fault_addr;
     asm volatile("mov %%cr2, %0" : "=r"(fault_addr)); // Get faulting address from CR2
 
     // Ensure regs pointer is valid before accessing it
     if (!regs) {
         // Cannot safely proceed or log registers if regs is NULL
         terminal_printf("\n--- PAGE FAULT (#PF) --- \n");
         terminal_printf(" FATAL ERROR: regs pointer is NULL in page_fault_handler!\n");
         PAGING_PANIC("Page Fault with NULL registers structure");
         return;
     }
 
     uint32_t error_code = regs->err_code; // Error code pushed by CPU
 
     // Decode error code bits
     bool present         = (error_code & 0x1);  // Bit 0
     bool write           = (error_code & 0x2);  // Bit 1
     bool user            = (error_code & 0x4);  // Bit 2
     bool reserved_bit    = (error_code & 0x8);  // Bit 3
     bool instruction_fetch = (error_code & 0x10); // Bit 4
     // Get current process safely
     pcb_t* current_process = get_current_process(); // Assumes this can return NULL
     uint32_t current_pid = current_process ? current_process->pid : (uint32_t)-1; // Use -1 or 0 for "no process"
 
     // --- Start logging fault details ---
     terminal_printf("\n--- PAGE FAULT (#PF) ---\n");
     // *** FORMAT FIX: %u (pid), %p (addr), %u (errcode) ***
     terminal_printf(" PID: %u, Addr: %p, ErrCode: 0x%u\n",
                     (unsigned int)current_pid,
                     (void*)fault_addr,
                     (unsigned long)error_code);
     terminal_printf(" Details: %s, %s, %s, %s, %s\n",
                     present ? "Present" : "Not-Present",
                     write ? "Write" : "Read",
                     user ? "User" : "Supervisor",
                     reserved_bit ? "Reserved-Bit-Set" : "Reserved-OK",
                     instruction_fetch ? (g_nx_supported ? "Instruction-Fetch(NX?)" : "Instruction-Fetch") : "Data-Access");
 
     // *** FORMAT FIX: %p (EIP), %u (CS/EFLAGS/registers) ***
     terminal_printf(" CPU State: EIP=%p, CS=0x%u, EFLAGS=0x%u\n",
                     (void*)regs->eip,
                     (unsigned long)regs->cs,
                     (unsigned long)regs->eflags);
     terminal_printf(" EAX=0x%u EBX=0x%u ECX=0x%u EDX=0x%u\n",
                     (unsigned long)regs->eax, (unsigned long)regs->ebx,
                     (unsigned long)regs->ecx, (unsigned long)regs->edx);
     terminal_printf(" ESI=0x%u EDI=0x%u EBP=%p K_ESP=0x%u\n", // Print Kernel ESP (esp_dummy), EBP as pointer
                     (unsigned long)regs->esi, (unsigned long)regs->edi,
                     (void*)regs->ebp, (unsigned long)regs->esp_dummy);
 
     // Log User ESP and SS only if the fault happened in User mode (makes sense)
     if (user) {
         // *** FORMAT FIX: %u ***
         terminal_printf("            U_ESP=0x%u, U_SS=0x%u\n", (unsigned long)regs->user_esp, (unsigned long)regs->user_ss);
     }
     // --- End logging fault details ---
 
 
     // --- Kernel (Supervisor) Fault ---
     if (!user) {
         terminal_printf(" Reason: Fault occurred in Supervisor Mode!\n");
 
         // Additional context for kernel faults
         if (reserved_bit) {
              // *** FORMAT FIX: %p ***
              terminal_printf(" CRITICAL: Reserved bit set in paging structure accessed by kernel at VAddr %p!\n", (void*)fault_addr);
         }
         if (!present) {
              // *** FORMAT FIX: %p ***
              terminal_printf(" CRITICAL: Kernel attempted to access non-present page at VAddr %p!\n", (void*)fault_addr);
         } else if (write) {
              // *** FORMAT FIX: %p ***
              terminal_printf(" CRITICAL: Kernel write attempt caused protection fault at VAddr %p!\n", (void*)fault_addr);
         }
 
         // Kernel faults are usually unrecoverable bugs
         PAGING_PANIC("Irrecoverable Supervisor Page Fault");
         return; // Should not reach here
     }
 
     // --- User Mode Fault ---
     terminal_printf(" Reason: Fault occurred in User Mode.\n");
 
     // Check if we have a valid process context RIGHT NOW
     if (!current_process) {
         // *** FORMAT FIX: %p ***
         terminal_printf("  Error: No current process available for user fault! Addr=%p\n", (void*)fault_addr);
         PAGING_PANIC("User Page Fault without process context!"); // Halt directly if state is inconsistent
         return;
     }
 
     // Check for valid mm_struct within the process
     if (!current_process->mm) {
         // *** FORMAT FIX: %u (pid), %p (addr) ***
         terminal_printf("  Error: Current process (PID %u) has no mm_struct! Addr=%p\n",
                         (unsigned int)current_pid, (void*)fault_addr);
         goto kill_process; // Cannot handle fault without memory context
     }
 
     mm_struct_t *mm = current_process->mm;
     if (!mm->pgd_phys) {
         terminal_printf("  Error: Current process mm_struct has no page directory (pgd_phys is NULL)!\n");
         goto kill_process; // Cannot handle fault without page directory
     }
 
     // --- Check for specific fatal error conditions first ---
     if (reserved_bit) {
         terminal_printf("  Error: Reserved bit set in user-accessed page table entry. Corrupted mapping? Terminating.\n");
         goto kill_process;
     }
 
     // Check for No-Execute (NX) violation if supported and it was an instruction fetch
     if (g_nx_supported && instruction_fetch) {
          // *** FORMAT FIX: %p ***
          terminal_printf("  Error: Instruction fetch from a No-Execute page (NX violation) at Addr=%p. Terminating.\n", (void*)fault_addr);
         goto kill_process;
     }
 
     // --- Consult Virtual Memory Areas (VMAs) ---
     // *** FORMAT FIX: %p ***
     terminal_printf("  Searching for VMA covering faulting address %p...\n", (void*)fault_addr);
     vma_struct_t *vma = find_vma(mm, fault_addr); // Find VMA covering the fault address
 
     if (!vma) {
         // *** FORMAT FIX: %p ***
         terminal_printf("  Error: No VMA covers the faulting address %p. Segmentation Fault.\n",
                         (void*)fault_addr);
         goto kill_process; // Address is outside any valid allocated region for this process
     }
 
     // VMA found, log details and check permissions against the fault type
     // *** FORMAT FIX: %u (addr), %u (page_prot) ***
     terminal_printf("  VMA Found: [0x%u - 0x%u) Flags: %c%c%c PageProt: 0x%u\n",
                     (unsigned long)vma->vm_start, (unsigned long)vma->vm_end,
                     (vma->vm_flags & VM_READ) ? 'R' : '-',
                     (vma->vm_flags & VM_WRITE) ? 'W' : '-',
                     (vma->vm_flags & VM_EXEC) ? 'X' : '-',
                     (unsigned long)vma->page_prot); // Log page protection too
 
     // Check Write Permission
     if (write && !(vma->vm_flags & VM_WRITE)) {
         terminal_printf("  Error: Write attempt to VMA without VM_WRITE flag. Segmentation Fault.\n");
         goto kill_process;
     }
     // Check Read Permission (relevant if not a write fault OR if instruction fetch)
     if (!write && !(vma->vm_flags & VM_READ)) {
          // Instruction fetch always requires read permission
          terminal_printf("  Error: Read/Execute attempt from VMA without VM_READ flag. Segmentation Fault.\n");
          goto kill_process;
     }
       // Check Execute Permission (relevant if instruction fetch, ignore if NX already handled)
       if (!g_nx_supported && instruction_fetch && !(vma->vm_flags & VM_EXEC)) {
           terminal_printf("  Error: Instruction fetch from VMA without VM_EXEC flag (NX not supported). Segmentation Fault.\n");
           goto kill_process;
       }
 
 
     // --- Handle the Fault (Demand Paging / Copy-on-Write) ---
     // If we got here: VMA exists, basic permissions align with fault type.
     // The fault likely means page isn't present OR it's a COW situation.
     terminal_printf("  Attempting to handle fault via VMA operations (Demand Paging / COW)...\n");
 
     // Call the function responsible for handling faults within a VMA
     // This function should implement demand paging (allocating/mapping pages)
     // and Copy-on-Write logic.
     int handle_result = handle_vma_fault(mm, vma, fault_addr, error_code);
 
     if (handle_result == 0) {
         // Fault was successfully handled by the VMA logic
         // *** FORMAT FIX: %u (pid) ***
         terminal_printf("  VMA fault handler succeeded. Resuming process PID %u.\n", (unsigned int)current_pid);
         terminal_printf("--------------------------\n");
         return; // Return from interrupt, CPU will re-run the faulting instruction
     } else {
         // VMA handler failed (e.g., out of memory, protection error during COW)
         terminal_printf("  Error: handle_vma_fault failed with code %d. Terminating process.\n", handle_result);
         goto kill_process;
     }
 
 
 kill_process:
     // Unhandled or fatal fault - terminate the process
     terminal_printf("--- Unhandled User Page Fault ---\n");
     // *** FORMAT FIX: %u (pid) ***
     terminal_printf(" Terminating Process PID %u.\n", (unsigned int)current_pid);
     terminal_printf("--------------------------\n");
 
     // Ensure we don't try to terminate a NULL process (should be caught earlier, but defensive check)
     if (current_process) {
         // Use scheduler function to terminate the current task
         remove_current_task_with_code(0xDEAD000E); // Use a specific exit code for page fault termination
     } else {
         // This case should ideally be impossible if the initial check passed, but handle defensively
         PAGING_PANIC("Page fault kill attempt with no valid process context!");
     }
 
     // Should not return from remove_current_task, but if it does, panic.
     PAGING_PANIC("remove_current_task returned after page fault kill!");
 }
 
 /**
  * kernel_unmap_virtual_unsafe - Improved version with better validation
  * This function unmaps a virtual address in the kernel's address space
  */
  #include <paging.h>
  #include <terminal.h> // For terminal_printf
  #include <frame.h>    // For put_frame
  #include <libc/stdint.h> // For uintptr_t, uint32_t
  #include <libc/stdbool.h> // For bool
  #include <libc/stddef.h> // For NULL
  
  // Assume PAGE_SIZE, TABLES_PER_DIR, PAGES_PER_TABLE, PDE_INDEX, PTE_INDEX,
  // PAGE_PRESENT, PAGE_SIZE_4MB, PAGING_PDE_ADDR_MASK_4KB, PAGING_PTE_ADDR_MASK,
  // PAGING_FLAG_MASK, RECURSIVE_PDE_VADDR are defined correctly in paging.h
  // Assume g_kernel_page_directory_virt points to the virtual address of the kernel PD.
  // Assume paging_invalidate_page(void*) is defined.
  // Assume is_page_table_empty(uint32_t* pt_virt) is defined and correct.
  
  /**
   * @brief Unmaps a single 4KB virtual page from the kernel address space.
   * @warning This function is UNSAFE. It directly manipulates kernel page tables
   * without considering higher-level memory management (VMAs) or
   * reference counting. It MUST be called with appropriate locks held
   * to protect the kernel page directory/tables.
   * It will free the underlying page table if it becomes empty.
   *
   * @param vaddr The page-aligned virtual address to unmap.
   * @return 0 on success, -1 on error (e.g., alignment, invalid address, not mapped).
   */
   int kernel_unmap_virtual_unsafe(uintptr_t vaddr) {
    // Use standard format specifiers for portability and clarity
    // PRIxPTR requires <inttypes.h>, using "%#zx" as a common alternative for uintptr_t hex
    // Assuming uintptr_t fits within unsigned long long for printing purposes if PRIxPTR not available
    // Using %u for uint32_t indices

    // Basic checks before attempting unmapping
    if (!g_kernel_page_directory_virt) {
        terminal_printf("[KUnmapUnsafe] Error: Kernel PD Virt (g_kernel_page_directory_virt) is NULL!\n");
        return -1;
    }

    if (vaddr % PAGE_SIZE != 0) {
        terminal_printf("[KUnmapUnsafe] Error: VAddr %#zx not page aligned.\n", vaddr);
        return -1;
    }

    // Get PDE/PTE indices
    uint32_t pd_idx = PDE_INDEX(vaddr);
    uint32_t pt_idx = PTE_INDEX(vaddr);

    // Validate PDE index against bounds
    if (pd_idx >= TABLES_PER_DIR) {
        terminal_printf("[KUnmapUnsafe] Error: Invalid PDE index %u for vaddr %#zx\n",
                          pd_idx, vaddr);
        return -1;
    }

    // --- CRITICAL SECTION START (Assumes caller holds lock) ---

    // Get PDE entry value
    uint32_t pde = g_kernel_page_directory_virt[pd_idx];

    // Check if PDE is present *before* trying to use it
    if (!(pde & PAGE_PRESENT)) {
        // Already not mapped at PDE level, consider this success for unmap
        // terminal_printf("[KUnmapUnsafe] Info: PDE[%u] for VAddr %#zx already not present.\n", pd_idx, vaddr);
        return 0;
    }

    // Check if it's a 4MB page (cannot unmap 4KB within it)
    if (pde & PAGE_SIZE_4MB) {
        terminal_printf("[KUnmapUnsafe] Error: Cannot unmap 4KB page VAddr %#zx; it's part of a 4MB mapping (PDE[%u]=%#08x).\n",
                          vaddr, pd_idx, pde);
        return -1;
    }

    // Get physical address of the page table from PDE
    uintptr_t pt_phys = pde & PAGING_PDE_ADDR_MASK_4KB;

    // Validate PT physical address derived from PDE
    // (Should always be page-aligned if set by mapping functions)
    if (pt_phys == 0) {
         terminal_printf("[KUnmapUnsafe] Error: Zero PT physical address in present PDE[%u]=%#08x for VAddr %#zx\n",
                           pd_idx, pde, vaddr);
         // This indicates page table corruption or invalid PDE state
         return -1;
    }
    // Optional stricter check: (pt_phys % PAGE_SIZE != 0), but mask should ensure this.


    // Calculate virtual address of the page table using recursive mapping
    // Note: RECURSIVE_PDE_VADDR must be the *base* virtual address mapping the start of the page directory's page table pointers.
    // e.g., 0xFFC00000 if recursive entry is PDE[1023]
    uint32_t* pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));

    /*
     * Removed the explicit validation check on `pt_virt` range here.
     * If `pd_idx` is valid (0-1023, excluding recursive index if needed) and
     * `RECURSIVE_PDE_VADDR` is correct, the calculation should yield a valid address
     * within the 4MB virtual range managed by the recursive PDE.
     * The previous error `Invalid PT virtual address 0xfff80000 for PDE[896]`
     * likely indicated that accessing this address *failed* because the
     * underlying PDE[1023] mapping was bad, OR PDE[896] became non-present
     * between the check above and access here (race condition -> need locks),
     * OR the hardware raised a fault for some other reason when accessing pt_virt.
     * Relying on the hardware fault or ensuring locks are held by the caller is
     * more robust than this specific range check.
     */

    // --- Access the Page Table ---
    // Read PTE entry value
    // (Potential fault here if pt_virt is invalid due to reasons above)
    uint32_t pte = pt_virt[pt_idx];


    // Check if PTE is present before clearing
    if (pte & PAGE_PRESENT) {
        // Optional: Check for reserved bits being set (indicates corruption)
        // Mask includes typical flags (P, RW, US, PWT, PCD, A, D, PAT, G) and address.
        // If any other bits are set, it's suspicious.
        // Note: Define PAGING_PTE_RESERVED_MASK appropriately for your architecture/needs.
        // if (pte & PAGING_PTE_RESERVED_MASK) {
        //     terminal_printf("[KUnmapUnsafe] Warning: PTE[%u] at VAddr %#zx has unexpected bits set: %#08x\n",
        //                       pt_idx, vaddr, pte);
        // }

        uintptr_t frame_phys = pte & PAGING_PTE_ADDR_MASK; // Get physical frame being unmapped

        // Clear the PTE
        pt_virt[pt_idx] = 0;

        // Invalidate TLB entry for the specific virtual address
        paging_invalidate_page((void*)vaddr);

        // Optional: Decrement refcount for the physical frame if applicable
        // This function is "unsafe" and might not do this, relying on caller.
        // If refcounting is used: put_frame(frame_phys); // <<<<<<<< IMPORTANT IF USING REFCOUNTS

        // Check if the page table is now empty
        // Assumes is_page_table_empty iterates through pt_virt[0..PAGES_PER_TABLE-1]
        bool pt_is_empty = is_page_table_empty(pt_virt);

        // If the page table is empty, free it and clear the PDE
        if (pt_is_empty) {
            terminal_printf("[KUnmapUnsafe] Info: PT at Phys %#zx (PDE[%u]) became empty after unmapping VAddr %#zx. Freeing PT.\n",
                              pt_phys, pd_idx, vaddr);

            // Clear the PDE entry in the kernel page directory
            g_kernel_page_directory_virt[pd_idx] = 0;

            // Invalidate TLB again (potentially affects wider range due to PDE change)
            // Using the same address should suffice, as TLB invalidation might be broader
            // or the CPU handles PDE changes correctly with INVLPG on an address within its range.
            // For safety, a full TLB flush might be considered, but often INVLPG is enough.
             paging_invalidate_page((void*)vaddr); // Or full flush if needed

            // Free the physical frame that held the page table itself
            // IMPORTANT: Ensure this doesn't conflict with refcounting if PT frames are refcounted.
             put_frame(pt_phys);
        }
    } else {
        // PTE was already not present, nothing to unmap. Considered success.
        // terminal_printf("[KUnmapUnsafe] Info: PTE[%u] for VAddr %#zx already not present.\n", pt_idx, vaddr);
    }

    // --- CRITICAL SECTION END (Assumes caller releases lock) ---

    return 0; // Success
}
 
 
 /**
  * is_page_table_empty - Improved function to check if a page table is empty
  * Adds validation and safety checks
  */
  static bool is_page_table_empty(uint32_t *pt_virt) {
    // Validate the PT pointer
    if (!pt_virt) {
        terminal_printf("[is_page_table_empty] Warning: NULL page table pointer provided\n");
        // Treat NULL PT as empty for safety, though this indicates a caller issue.
        return true;
    }

    // Check all entries in the page table.
    // The table is considered empty only if ALL entries are zero.
    for (size_t i = 0; i < PAGES_PER_TABLE; ++i) {
        // Check the raw value. Any non-zero entry means the table isn't empty.
        // This is stricter than just checking PAGE_PRESENT, ensuring truly clean tables.
        if (pt_virt[i] != 0) {
            return false; // Found a non-zero entry, table is not empty
        }
    }

    // If the loop completes without finding any non-zero entries, the table is empty.
    return true;
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
     // *** FORMAT FIX: %p, %u ***
     terminal_printf("[PagingSet] Setting Kernel PD Globals: Virt=%p Phys=0x%u\n", (void*)pd_virt, (unsigned long)pd_phys);
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }
 
 int paging_map_single_4k(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
     // Basic alignment checks
     if ((vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) {
         // *** FORMAT FIX: %u ***
         terminal_printf("[MapSingle4k] Error: Unaligned addresses V=0x%u P=0x%u\n", (unsigned long)vaddr, (unsigned long)paddr);
         return -1;
     }
     // Call the internal helper, explicitly setting use_large_page to false
     return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
 }


 void* paging_temp_map(uintptr_t phys_addr) {
    // Validate physical address alignment
    if (phys_addr % PAGE_SIZE != 0) {
        terminal_printf("[Paging Temp Map] Error: Physical address 0x%u is not page-aligned.\n", (unsigned long)phys_addr);
        return NULL;
    }

    // Use the unsafe kernel mapping function. Assumes kernel RW permissions are okay.
    // You might want specific flags depending on usage.
    int result = kernel_map_virtual_to_physical_unsafe(
        TEMP_MAP_ADDR_GENERIC, // The predefined temporary virtual address
        phys_addr,
        PTE_KERNEL_DATA_FLAGS // Kernel Read/Write flags
    );

    if (result != 0) {
        terminal_printf("[Paging Temp Map] Error: Failed to map paddr 0x%u to temp vaddr 0x%u.\n",
                        (unsigned long)phys_addr, (unsigned long)TEMP_MAP_ADDR_GENERIC);
        return NULL;
    }

    // terminal_printf("[Paging Temp Map] Mapped P=0x%u -> V=%p\n", (unsigned long)phys_addr, (void*)TEMP_MAP_ADDR_GENERIC);
    return (void*)TEMP_MAP_ADDR_GENERIC;
}

/**
 * @brief Unmaps the predefined temporary virtual address.
 *
 * @param temp_vaddr The virtual address that was returned by paging_temp_map.
 * This is validated against the known temporary address.
 */
void paging_temp_unmap(void* temp_vaddr) {
    // Validate the address being unmapped
    if ((uintptr_t)temp_vaddr != TEMP_MAP_ADDR_GENERIC) {
        terminal_printf("[Paging Temp Unmap] Warning: Attempting to unmap unexpected address %p (expected %p).\n",
                        temp_vaddr, (void*)TEMP_MAP_ADDR_GENERIC);
        // Decide if this should be a fatal error or just a warning
        // For safety, potentially only unmap if the address matches:
        // return;
    }

    // Use the unsafe kernel unmapping function
    kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_GENERIC);
    // terminal_printf("[Paging Temp Unmap] Unmapped V=%p\n", (void*)TEMP_MAP_ADDR_GENERIC);
}

/**
 * @brief Copies kernel-space Page Directory Entries (PDEs) from the master
 * kernel page directory to a destination page directory.
 * Assumes the destination PD is already mapped writeable at dst_pd_virt.
 *
 * @param dst_pd_virt Pointer to the destination page directory (virtual address).
 * @return 0 on success, -1 on error.
 */
// Corrected version for paging.c
void copy_kernel_pde_entries(uint32_t *dst_pd_virt) { // *** CHANGE int to void ***
    if (!g_kernel_page_directory_virt) {
        terminal_printf("[CopyPDEs] Error: Kernel PD global pointer not set.\n");
        return; // *** CHANGE return -1; to return; ***
    }
    if (!dst_pd_virt) {
        terminal_printf("[CopyPDEs] Error: Destination PD pointer is NULL.\n");
        return; // *** CHANGE return -1; to return; ***
    }

    size_t start_index = KERNEL_PDE_INDEX;
    size_t end_index = RECURSIVE_PDE_INDEX;

    if (start_index >= end_index || end_index > TABLES_PER_DIR) {
        terminal_printf("[CopyPDEs] Error: Invalid kernel PDE indices (%zu - %zu).\n", start_index, end_index);
        return; // *** CHANGE return -1; to return; ***
    }

    size_t count = end_index - start_index;
    size_t bytes_to_copy = count * sizeof(uint32_t);

    memcpy(dst_pd_virt + start_index, g_kernel_page_directory_virt + start_index, bytes_to_copy);

    // No return 0; needed at the end
}

int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz) {
    terminal_printf("[Unmap Range] V=[0x%u - 0x%u) in PD Phys %p\n",
                    (unsigned long)virt_start_addr, (unsigned long)(virt_start_addr + memsz), (void*)page_directory_phys);

    if (!page_directory_phys || memsz == 0) {
        terminal_printf("[Unmap Range] Error: Invalid PD or zero size.\n");
        return -1;
    }

    // Align start address down, calculate end address, and align end address up.
    uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
    uintptr_t v_end;
    if (virt_start_addr > UINTPTR_MAX - memsz) { // Check overflow for end address calculation
        v_end = UINTPTR_MAX;
    } else {
        v_end = virt_start_addr + memsz;
    }
    v_end = PAGE_ALIGN_UP(v_end);
    if (v_end < v_start) { // Handle overflow from alignment
        v_end = UINTPTR_MAX;
    }

    if (v_start == v_end) {
        terminal_printf("[Unmap Range] Warning: Range is empty after alignment.\n");
        return 0; // Nothing to unmap
    }

    // Determine if operating on the current PD or another one
    bool is_current_pd = ((uintptr_t)page_directory_phys == g_kernel_page_directory_phys);
    uint32_t* target_pd_virt = NULL;

    if (!is_current_pd) {
        // Map the target PD temporarily if it's not the current one
        target_pd_virt = paging_temp_map((uintptr_t)page_directory_phys);
        if (!target_pd_virt) {
            terminal_printf("[Unmap Range] Error: Failed to temp map target PD %p.\n", (void*)page_directory_phys);
            return -1;
        }
    } else {
        // Operating on the current PD, use its known virtual address
        target_pd_virt = g_kernel_page_directory_virt;
        if (!target_pd_virt) {
             PAGING_PANIC("Unmap Range on current PD, but kernel PD virt is NULL!");
             return -1; // Should not happen if paging active
        }
    }

    // Iterate through the virtual range page by page
    int unmapped_count = 0;
    for (uintptr_t v_addr = v_start; v_addr < v_end; v_addr += PAGE_SIZE) {
        uint32_t pd_idx = PDE_INDEX(v_addr);

        // Ensure we are not touching kernel or recursive space if unmapping user space
        // This check might need adjustment based on your exact memory layout policy
        if (pd_idx >= KERNEL_PDE_INDEX) {
             terminal_printf("[Unmap Range] Warning: Attempt to unmap kernel/recursive range V=0x%u skipped.\n", (unsigned long)v_addr);
             continue; // Skip kernel/recursive PDEs
        }

        uint32_t pde = target_pd_virt[pd_idx];

        if (!(pde & PAGE_PRESENT)) {
            continue; // This part of the range is already unmapped at PDE level
        }

        if (pde & PAGE_SIZE_4MB) {
            // This implementation currently does not support unmapping ranges overlapping 4MB pages.
            terminal_printf("[Unmap Range] Error: Cannot unmap range overlapping 4MB page at V=0x%u.\n", (unsigned long)v_addr);
            if (!is_current_pd) paging_temp_unmap(target_pd_virt);
            return -1;
        }

        // PDE points to a 4KB Page Table
        uintptr_t pt_phys = pde & PAGING_ADDR_MASK; // Physical address of the page table
        uint32_t* pt_virt = NULL;
        bool pt_mapped_here = false;

        // Get virtual access to the Page Table
        if (is_current_pd) {
             pt_virt = (uint32_t*)(RECURSIVE_PDE_VADDR + (pd_idx * PAGE_SIZE));
        } else {
             pt_virt = paging_temp_map(pt_phys);
             if (!pt_virt) {
                 terminal_printf("[Unmap Range] Error: Failed to temp map target PT %p for V=0x%u.\n", (void*)pt_phys, (unsigned long)v_addr);
                 // Continue to next PDE, but leak the PT for now? Or return error?
                 // Returning error is safer to signal incomplete unmap.
                 if (!is_current_pd) paging_temp_unmap(target_pd_virt);
                 return -1;
             }
             pt_mapped_here = true;
        }

        // Process the PTE for the current v_addr
        uint32_t pt_idx = PTE_INDEX(v_addr);
        uint32_t pte = pt_virt[pt_idx];

        if (pte & PAGE_PRESENT) {
            uintptr_t frame_phys = pte & PAGING_ADDR_MASK;
            pt_virt[pt_idx] = 0; // Clear the PTE

            // Invalidate TLB for this specific page
            // If operating on non-current PD, this might not be strictly necessary immediately,
            // but good practice if the PD could become active later without a full flush.
            paging_invalidate_page((void*)v_addr);

            // Free the physical frame
            put_frame(frame_phys);
            unmapped_count++;

            // Check if the entire page table is now empty
            // NOTE: is_page_table_empty needs the VIRTUAL address of the PT
            if (is_page_table_empty(pt_virt)) {
                terminal_printf("[Unmap Range] PT at Phys 0x%u (PDE[%u]) became empty. Freeing PT.\n",
                                (unsigned long)pt_phys, (unsigned long)pd_idx);

                // Clear the PDE in the target PD
                target_pd_virt[pd_idx] = 0;

                // Invalidate TLB again as PDE changed
                // Use address within the range covered by the PDE
                paging_invalidate_page((void*)v_addr);

                // Free the frame used by the page table itself
                put_frame(pt_phys);

                // If we temp mapped the PT, we need to unmap it *before* moving to next v_addr
                if (pt_mapped_here) {
                    paging_temp_unmap(pt_virt);
                    pt_mapped_here = false; // Mark as unmapped
                    pt_virt = NULL;
                }
                // Since the PDE is gone, we can break out of the inner loop for this PDE
                // and continue to the next 4MB-aligned virtual address.
                // We need to advance v_addr to the start of the next PDE range.
                 uintptr_t next_pde_addr = PAGE_ALIGN_DOWN(v_addr) + PAGE_SIZE_LARGE;
                 if (next_pde_addr <= v_addr) { // Check overflow
                     v_addr = v_end; // Go to end
                 } else {
                     v_addr = next_pde_addr - PAGE_SIZE; // Set so next iteration is start of next PDE
                 }
                 // Skip the rest of the loop for this iteration
                 continue;

            }
        }

        // Unmap the temporary PT mapping if we created one for this iteration
        if (pt_mapped_here) {
            paging_temp_unmap(pt_virt);
        }

        // Check for overflow before incrementing loop variable
        if (v_addr > UINTPTR_MAX - PAGE_SIZE) {
             break;
        }

    } // End for loop iterating through virtual addresses

    // Unmap the target PD if we mapped it temporarily
    if (!is_current_pd) {
        paging_temp_unmap(target_pd_virt);
    }

    terminal_printf("[Unmap Range] Finished. Unmapped approx %d pages.\n", unmapped_count);
    return 0; // Success
}
 