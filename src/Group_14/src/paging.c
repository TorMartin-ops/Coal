/**
 * paging.c - Paging Implementation with PSE (4MB Pages) support.
 *
 * REVISED (v2): Separates initialization into stages to handle the dependency
 * between the buddy allocator needing mapped memory and paging needing the buddy allocator.
 * This version uses a pre-allocated page directory frame (or you may allocate one via paging_alloc_early_frame())
 * and integrates all of the full mapping/unmapping logic from the older version.
 */

 #include "paging.h"
 #include "frame.h"          // Needed for put_frame (used after paging is active)
 #include "buddy.h"          // For BUDDY_ALLOC and BUDDY_FREE (for PTs, post-buddy_init)
 #include "terminal.h"       // For logging and KERNEL_PANIC_HALT definition
 #include "types.h"          // For uintptr_t, uint32_t, size_t, bool
 #include "process.h"        // For page_fault_handler context and get_current_process
 #include "mm.h"             // For find_vma, handle_vma_fault in page fault handler
 #include "scheduler.h"      // For remove_current_task_with_code in page fault handler
 #include <string.h>         // For memset
 #include "cpuid.h"          // For cpuid wrapper
 #include "kmalloc_internal.h" // For ALIGN_UP macro
 #include "multiboot2.h"     // Needed for early frame allocation

 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL; // Will be set in stage 3
 uint32_t g_kernel_page_directory_phys = 0;     // Ditto
 bool g_pse_supported = false;                  // True if CPU supports 4MB pages

 // --- Globals Required for Early Frame Allocation ---
 // Assumed to be set in kernel.c:main and made available via kernel.c
 extern uint32_t g_multiboot_info_phys_addr_global;
 // Linker symbols defining kernel physical location
 extern uint8_t _kernel_start_phys;
 extern uint8_t _kernel_end_phys;


 // Temporary virtual addresses used for mapping PD/PT after paging is active.
 // These addresses must be reserved in your kernel layout.
 #ifndef TEMP_MAP_ADDR_PT
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PD
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
 #endif

 // --- Helper Macros --- (ensure these are defined appropriately in paging.h or types.h)
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
 #endif
 #ifndef PAGE_SIZE_LARGE
 #define PAGE_SIZE_LARGE (4 * 1024 * 1024)
 #endif
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
 #ifndef PAGE_ALIGN_DOWN
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #endif
 #ifndef PAGE_ALIGN_UP
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 #endif
 #define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
 #define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))

 // Flags for page entries (ensure these are defined in paging.h or types.h)
 #define PAGE_PRESENT   0x001
 #define PAGE_RW        0x002
 #define PAGE_USER      0x004
 #define PAGE_SIZE_4MB  0x080

 #define PTE_KERNEL_DATA_FLAGS (PAGE_PRESENT | PAGE_RW)
 #define PTE_USER_DATA_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER)

 // Kernel panic for paging errors (prints message then halts the system)
 #define PAGING_PANIC(msg) do { \
     terminal_printf("\n[PAGING PANIC] %s. System Halted.\n", msg); \
     while (1) { asm volatile("cli; hlt"); } \
 } while(0)


 // --- Early Allocation Tracking ---
 // Simple static array to keep track of frames allocated before the buddy/frame allocators are ready.
 // Adjust MAX_EARLY_ALLOCATIONS based on how many page tables might be needed
 // during paging_setup_early_maps. 32 should be plenty for mapping kernel/heap/VGA.
 #define MAX_EARLY_ALLOCATIONS 32
 static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
 static int early_allocated_count = 0;


 // --- Forward Declarations ---
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static uint32_t* allocate_page_table_phys_buddy(void);
 static uintptr_t paging_alloc_early_pt_frame_physical(void); // Forward declare the function we are adding
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type); // Declare helper


 // --- Helper to find Multiboot tag (copied/adapted from kernel.c logic) ---
 // This needs access to physical memory where the multiboot info resides.
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
    // Basic validation (assuming info struct is below 1MB and accessible)
    if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) {
        // Cannot use terminal_printf if console isn't ready, rely on return code
        return NULL;
    }
    // Access physical memory directly - requires appropriate mapping or runs before paging
    uint32_t total_size = *(uint32_t*)mb_info_phys_addr;
    // Basic size sanity check (e.g., max 1MB for info struct size)
    if (total_size < 8 || total_size > 0x100000) {
        return NULL;
    }

    struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
    uintptr_t info_end = mb_info_phys_addr + total_size;

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        uintptr_t current_tag_addr = (uintptr_t)tag;
        // Bounds check tag header
        if (current_tag_addr + 8 > info_end) return NULL;
        // Bounds check tag content
        if (tag->size < 8 || (current_tag_addr + tag->size) > info_end) return NULL;

        if (tag->type == type) return tag; // Found

        // Advance to the next tag (8-byte alignment)
        uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
        // Check bounds before advancing
        if (next_tag_addr >= info_end) break;
        tag = (struct multiboot_tag *)next_tag_addr;
    }
    return NULL; // End tag reached or error
 }

/**
 * @brief Allocates a physical frame for early Page Table use.
 *
 * Scans the Multiboot memory map for the first available page frame >= 1MB
 * that doesn't conflict with the kernel image or previously allocated early frames.
 * Zeros the allocated frame before returning.
 * WARNING: This function is intended for use ONLY during initial paging setup
 * BEFORE the main frame allocator (frame.c/buddy.c) is initialized. It does NOT
 * use the main allocators and relies on a simple static tracking mechanism.
 * It assumes physical memory addresses >= 1MB are accessible for zeroing.
 *
 * @return Physical address of a zeroed 4KB page frame, or 0 on failure.
 */
static uintptr_t paging_alloc_early_pt_frame_physical(void) {
    // Ensure multiboot info address is valid (should be set by kernel entry)
    if (g_multiboot_info_phys_addr_global == 0) {
         // Cannot print reliably here if terminal requires mapping not yet done.
         // Returning 0 indicates critical failure.
         return 0;
    }

    // Find the memory map tag using the physical address
    struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag_early(
        g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);

    if (!mmap_tag) {
        // Cannot print reliably. Return 0 for failure.
        return 0;
    }

    // Get kernel physical boundaries
    uintptr_t kernel_start_p = (uintptr_t)&_kernel_start_phys;
    uintptr_t kernel_end_p = (uintptr_t)&_kernel_end_phys;

    multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
    uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size; // Calculate end of tag data

    while ((uintptr_t)mmap_entry < mmap_end) {
        // Basic validation of entry structure before use
        if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t)) {
             // Invalid entry size, cannot proceed safely.
             break;
        }

        // Check if the region is marked as available by the bootloader
        if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
            uintptr_t region_start = (uintptr_t)mmap_entry->addr;
            uint64_t region_len_64 = mmap_entry->len; // Use 64-bit for length calculation
            uintptr_t region_end = region_start + (uintptr_t)region_len_64; // Calculate end, handle potential overflow if addr is high

             // Clamp region_end to prevent overflow issues with uintptr_t if region is huge
             if (region_end < region_start) { // Overflow occurred
                 region_end = UINTPTR_MAX;
             }

            // Iterate through page frames within this available region
            uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);

            while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr /* Check overflow */ ) {

                // Criteria 1: Must be >= 1MB
                if (current_frame_addr < 0x100000) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                // Criteria 2: Must not overlap with the kernel image
                bool overlaps_kernel = (current_frame_addr < kernel_end_p && (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                if (overlaps_kernel) {
                    current_frame_addr += PAGE_SIZE;
                    continue;
                }

                // Criteria 3: Must not have been allocated by *this* function already
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

                // --- Found a suitable frame ---
                if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) {
                     // Cannot allocate more frames using this early method.
                     // This indicates a potential issue or need for more robust early allocation.
                     // Cannot reliably print here.
                     return 0;
                }

                // Record the allocation
                early_allocated_frames[early_allocated_count++] = current_frame_addr;

                // Zero the frame content.
                // ASSUMPTION: Physical address `current_frame_addr` is accessible here.
                // This requires identity mapping or careful setup in early boot.
                memset((void*)current_frame_addr, 0, PAGE_SIZE);

                // Return the physical address of the allocated and zeroed frame
                // Can print here as console is likely mapped by the time this is called during setup_early_maps
                // terminal_printf("  [Paging Early Alloc] Allocated PT frame: Phys=0x%x\n", current_frame_addr);
                return current_frame_addr;

            } // End while loop through frames in the region
        } // End if region is available

        // Advance to the next memory map entry
        uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
        // Check bounds before dereferencing next entry
        if (next_entry_addr > mmap_end) break;
        mmap_entry = (multiboot_memory_map_t *)next_entry_addr;

    } // End while loop through memory map entries

    // If we finish the loop without returning, no suitable frame was found.
    // Cannot print reliably. Return 0 for failure.
    return 0;
}


 // --- CPU Feature Detection and Control ---
 static inline uint32_t read_cr4(void) {
     uint32_t v;
     asm volatile("mov %%cr4, %0" : "=r"(v));
     return v;
 }
 static inline void write_cr4(uint32_t v) {
     asm volatile("mov %0, %%cr4" :: "r"(v));
 }
 static inline void enable_cr4_pse(void) {
     write_cr4(read_cr4() | (1 << 4));  // Set CR4.PSE (bit 4)
 }

 /**
  * @brief Check for PSE (4MB page) support and enable it.
  * @return true if supported and enabled; false otherwise.
  */
 bool check_and_enable_pse(void) {
     uint32_t eax, ebx, ecx, edx;
     cpuid(1, &eax, &ebx, &ecx, &edx);

     if (edx & (1 << 3)) {  // PSE supported if EDX bit 3 is set
         terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
         enable_cr4_pse();
         if (read_cr4() & (1 << 4)) {
             terminal_write("[Paging] CR4.PSE bit enabled.\n");
             g_pse_supported = true;
             return true;
         } else {
             terminal_write("[Paging Error] Failed to enable CR4.PSE bit after check!\n");
         }
     } else {
         terminal_write("[Paging] CPU does not support PSE (4MB Pages).\n");
     }
     g_pse_supported = false;
     return false;
 }

 // --- Public API Functions ---

 /**
  * @brief Set the global kernel page directory pointers.
  * @param pd_virt Virtual address of the PD.
  * @param pd_phys Physical address of the PD.
  */
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }

 /**
  * @brief Invalidate the TLB entry for a specific virtual address.
  * @param vaddr Address to invalidate.
  */
 void paging_invalidate_page(void *vaddr) {
     asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }

 /**
  * @brief Flush the entire TLB for a range.
  * @param start Starting virtual address.
  * @param size Size (in bytes) of the range.
  */
 void tlb_flush_range(void* start, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr = PAGE_ALIGN_UP((uintptr_t)start + size);
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr);
         addr += PAGE_SIZE;
     }
 }

 /**
  * @brief Activate paging by loading the given PD’s physical address into CR3
  * and setting the PG bit in CR0.
  * @param page_directory_phys Physical address of the PD.
  */
 void paging_activate(uint32_t *page_directory_phys) {
     uint32_t cr0;
     asm volatile("mov %0, %%cr3" : : "r"(page_directory_phys) : "memory");
     asm volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit
     asm volatile("mov %0, %%cr0" : : "r"(cr0));
 }

 /**
  * @brief Allocate an early physical frame for the page directory (or for PTs)
  * using the buddy allocator. The frame is zeroed before returning.
  * @return The physical address of the allocated frame or 0 on failure.
  */
 uintptr_t paging_alloc_early_frame() {
     terminal_write("  [Paging WARNING] Attempting early frame allocation via BUDDY for PD...\n");
     void* frame_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!frame_ptr) {
         terminal_write("  [Paging FATAL] BUDDY_ALLOC failed for initial PD frame!\n");
         return 0;
     }
     uintptr_t frame_phys = (uintptr_t)frame_ptr;
     terminal_printf("  [Paging DEBUG] Zeroing early frame at physical address 0x%x\n", frame_phys);
     memset((void*)frame_phys, 0, PAGE_SIZE);
     terminal_printf("  [Paging] Allocated early frame at phys 0x%x\n", frame_phys);
     return frame_phys;
 }

 /**
 * @brief Internal helper to map physical memory to virtual memory before paging is fully active.
 *
 * Maps the physical address range [phys_addr_to_map, phys_addr_to_map + size)
 * to a corresponding virtual address range.
 * If map_to_higher_half is true, maps to KERNEL_SPACE_VIRT_START + phys_addr_to_map.
 * If map_to_higher_half is false, performs an identity map (virt == phys).
 *
 * Requires direct physical access to the page directory and page table frames being modified.
 * Uses paging_alloc_early_pt_frame_physical() to allocate new page table frames if needed.
 *
 * @param page_directory_phys Physical address of the page directory being modified.
 * @param phys_addr_to_map Starting physical address to map.
 * @param size Size of the region to map in bytes.
 * @param flags PTE flags to use (e.g., PTE_KERNEL_DATA_FLAGS).
 * @param map_to_higher_half True to map into higher half, false for identity map.
 * @return 0 on success, negative on error.
 */
 static int paging_map_physical(uint32_t *page_directory_phys, uintptr_t phys_addr_to_map, size_t size, uint32_t flags, bool map_to_higher_half) {
    if (!page_directory_phys || size == 0) return -1;

    uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_to_map);
    uintptr_t end_phys = PAGE_ALIGN_UP(phys_addr_to_map + size); // Non-inclusive end

    flags |= PAGE_PRESENT; // Ensure pages are marked present

    while (current_phys < end_phys) {
        uintptr_t target_vaddr = map_to_higher_half ? (KERNEL_SPACE_VIRT_START + current_phys) : current_phys;
        uint32_t pd_idx = PDE_INDEX(target_vaddr);
        if (pd_idx >= 1024) {
            terminal_printf("  [PhysMap ERROR] Invalid PDE Index %u for V=0x%x\n", pd_idx, target_vaddr);
            return -1;
        }

        // Access PDE directly via physical address (mapped identity or accessible)
        uint32_t* pd_entry_ptr = &page_directory_phys[pd_idx];
        uint32_t pde = *pd_entry_ptr;
        uint32_t* pt_phys_ptr = NULL; // PHYSICAL pointer to PT

        if ((pde & PAGE_PRESENT) && (pde & PAGE_SIZE_4MB)) {
             terminal_printf("  [PhysMap ERROR] Conflict: PDE %d (V=0x%x) already maps 4MB page.\n", pd_idx, target_vaddr);
             return -1;
        }

        if (!(pde & PAGE_PRESENT)) {
            // Page Table not present, allocate one using the early allocator
            uintptr_t pt_frame_phys = paging_alloc_early_pt_frame_physical();
            if (!pt_frame_phys) {
                terminal_printf("  [PhysMap ERROR] Failed alloc EARLY PT frame for PDE %d (V=0x%x)\n", pd_idx, target_vaddr);
                return -1;
            }
            pt_phys_ptr = (uint32_t*)pt_frame_phys; // PT address is physical

            // Update the PDE entry (at its physical address)
            uint32_t pde_flags = PAGE_PRESENT | PAGE_RW; // Kernel needs RW access to PT
            if (flags & PAGE_USER) pde_flags |= PAGE_USER; // Inherit USER flag if needed
            *pd_entry_ptr = (pt_frame_phys & ~0xFFF) | pde_flags;
            // No TLB flush needed here, paging isn't active yet

        } else {
            // Page Table exists, get its physical address
            pt_phys_ptr = (uint32_t*)(pde & ~0xFFF);
            // Ensure PDE flags allow access needed by PTE flags
            uint32_t needed_pde_flags = PAGE_PRESENT | PAGE_RW;
            if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;
            if ((pde & needed_pde_flags) != needed_pde_flags) {
                 *pd_entry_ptr |= (flags & (PAGE_RW | PAGE_USER)); // Promote PDE flags if necessary
            }
        }

        // Access PTE directly via physical address (mapped identity or accessible)
        uint32_t pt_idx = PTE_INDEX(target_vaddr);
        if (pt_idx >= 1024) {
             terminal_printf("  [PhysMap ERROR] Invalid PTE Index %u for V=0x%x\n", pt_idx, target_vaddr);
             return -1;
        }
        uint32_t* pt_entry_ptr = &pt_phys_ptr[pt_idx]; // PHYSICAL pointer to PTE

        if (*pt_entry_ptr & PAGE_PRESENT) {
             // Log potential overwrite only if necessary for debugging
             // terminal_printf("  [PhysMap WARNING] Overwriting PTE for V=0x%x (P=0x%x) was 0x%x -> 0x%x\n",
             //                 target_vaddr, current_phys, *pt_entry_ptr, (current_phys & ~0xFFF) | flags);
        }

        // Set the PTE entry (at its physical address)
        *pt_entry_ptr = (current_phys & ~0xFFF) | flags;

        // Advance to the next page frame
        if (current_phys > UINTPTR_MAX - PAGE_SIZE) break; // Prevent overflow
        current_phys += PAGE_SIZE;
    }
    return 0; // Success
 }

 /**
  * @brief Stage 1: Initialize the Page Directory.
  *
  * @param initial_pd_phys A pre-allocated, page-aligned physical address for the PD.
  * (Alternatively, you can call paging_alloc_early_frame() to get one.)
  * @return The validated PD physical address, or 0 on failure.
  */
 uintptr_t paging_initialize_directory(uintptr_t initial_pd_phys) {
     terminal_write("[Paging Stage 1] Configuring Page Directory...\n");
     if (initial_pd_phys == 0 || (initial_pd_phys % PAGE_SIZE != 0)) {
         terminal_write("  [Paging FATAL] Invalid physical PD address provided!\n");
         return 0;
     }
     // Enable PSE (needed for 4MB page mappings later)
     check_and_enable_pse();
     // (Zeroing of the PD frame should be done by the caller or during early mapping.)
     terminal_printf("  [Paging Stage 1] PD structure configured at phys 0x%x.\n", initial_pd_phys);
     return initial_pd_phys;
 }

 /**
  * @brief Stage 2: Setup Early Mappings (PRE-PAGING).
  * Establishes identity mappings for the buddy heap and kernel region,
  * maps the kernel into the higher half, maps VGA memory, and maps the PD itself.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param kernel_phys_start Physical start address of the kernel.
  * @param kernel_phys_end Physical end address of the kernel.
  * @param heap_phys_start Physical start address of the buddy heap.
  * @param heap_size Size of the buddy heap.
  * @return 0 on success, negative on failure.
  */
 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
                             uintptr_t heap_phys_start, size_t heap_size)
 {
     terminal_write("[Paging Stage 2] Setting up early physical mappings...\n");
     if (page_directory_phys == 0) return -1;
     uint32_t *pd_phys_ptr = (uint32_t*)page_directory_phys;

     // --- Identity Map Buddy Heap Region ---
     terminal_printf("  Identity mapping Buddy Heap [Phys: 0x%x - 0x%x)\n", heap_phys_start, heap_phys_start + heap_size);
     if (paging_map_physical(pd_phys_ptr, heap_phys_start, heap_size, PTE_KERNEL_DATA_FLAGS, false) != 0) {
         PAGING_PANIC("Failed to identity map buddy heap region!");
         return -1;
     }

     // --- Identity Map Kernel Region ---
     terminal_printf("  Identity mapping Kernel [Phys: 0x%x - 0x%x)\n", kernel_phys_start, kernel_phys_end);
     if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA_FLAGS, false) != 0) {
         PAGING_PANIC("Failed to identity map kernel region!");
         return -1;
     }

     // --- Map Kernel to Higher Half ---
     terminal_printf("  Mapping Kernel to Higher Half [Phys: 0x%x -> Virt: 0x%x, Size: %u)\n",
                     kernel_phys_start, KERNEL_SPACE_VIRT_START + kernel_phys_start, kernel_phys_end - kernel_phys_start);
     if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA_FLAGS, true) != 0) {
         PAGING_PANIC("Failed to map kernel to higher half!");
         return -1;
     }

     // --- Map VGA Memory ---
     terminal_write("  Mapping VGA Memory [Phys: 0xB8000 -> Virt: 0xC00B8000)\n");
     if (paging_map_physical(pd_phys_ptr, 0xB8000, PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true) != 0) {
          PAGING_PANIC("Failed to map VGA memory!");
          return -1;
     }

     // --- Map the Page Directory itself to higher half ---
     terminal_printf("  Mapping Page Directory self [Phys: 0x%x -> Virt: 0x%x)\n", page_directory_phys, KERNEL_SPACE_VIRT_START + page_directory_phys);
     if (paging_map_physical(pd_phys_ptr, page_directory_phys, PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true) != 0) {
         PAGING_PANIC("Failed to map Page Directory to higher half!");
         return -1;
     }

     terminal_write("  [Paging Stage 2] Early maps established.\n");
     return 0;
 }

 /**
  * @brief Stage 3: Finalize Mappings and Activate Paging (POST-BUDDY_INIT).
  * Sets global PD pointers, ensures a self-reference mapping, maps
  * all physical memory into the higher half and activates paging.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param total_memory_bytes Total physical memory size to map.
  * @return 0 on success, negative on failure.
  */
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes) {
     terminal_write("[Paging Stage 3] Finalizing mappings and activating...\n");
     if (page_directory_phys == 0) return -1;

     // --- Set Global PD Pointers ---
     uintptr_t pd_virt_addr = KERNEL_SPACE_VIRT_START + page_directory_phys;
     terminal_printf("  Setting global PD pointers: Phys=0x%x, Virt=0x%x\n", page_directory_phys, pd_virt_addr);
     paging_set_kernel_directory((uint32_t*)pd_virt_addr, page_directory_phys);
     if (g_kernel_page_directory_phys != page_directory_phys || g_kernel_page_directory_virt != (uint32_t*)pd_virt_addr) {
          PAGING_PANIC("Failed to set global PD pointers correctly!");
          return -1;
     }

     // --- Ensure Page Directory Self-Mapping ---
     // Note: This was already done in Stage 2 by paging_map_physical
     terminal_write("  Ensuring Page Directory self-mapping (already done in Stage 2)...\n");
     // if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, pd_virt_addr, page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
     //      terminal_write("  [FATAL] Failed to map PD into virtual address space!\n");
     //      g_kernel_page_directory_phys = 0;
     //      g_kernel_page_directory_virt = NULL;
     //      return -1;
     // }

     // --- Map ALL Physical Memory to Higher Half ---
     terminal_printf("  Mapping ALL physical memory to higher half [Phys: 0x0 - 0x%x -> Virt: 0x%x - 0x%x)\n",
                     total_memory_bytes, KERNEL_SPACE_VIRT_START, KERNEL_SPACE_VIRT_START + total_memory_bytes);
     // Note: This will use the buddy allocator internally via map_page_internal -> allocate_page_table_phys_buddy
     if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, KERNEL_SPACE_VIRT_START, 0, total_memory_bytes, PTE_KERNEL_DATA_FLAGS) != 0) {
          PAGING_PANIC("Failed to map physical memory to higher half!");
          g_kernel_page_directory_phys = 0;
          g_kernel_page_directory_virt = NULL;
          return -1;
     }

     // --- Activate Paging ---
     terminal_write("  Activating Paging...\n");
     paging_activate((uint32_t*)page_directory_phys);
     terminal_write("  [OK] Paging Enabled.\n");

     // --- Post-Activation Sanity Check ---
     if (!g_kernel_page_directory_virt)
          PAGING_PANIC("Kernel PD virtual pointer is NULL after activation!");
     // Access the PD via its *virtual* address now
     terminal_printf("  Post-activation read PDE[Self]: 0x%x\n", g_kernel_page_directory_virt[PDE_INDEX((uintptr_t)g_kernel_page_directory_virt)]);

     terminal_write("[Paging Stage 3] Finalization complete.\n");
     return 0;
 }

 // --- Functions Below Operate AFTER Paging is Active ---

 /**
  * @brief Helper to allocate and zero a new Page Table frame using the buddy allocator.
  * The allocated PT is temporarily mapped using the kernel PD virtual pointer.
  * @return Physical address of the new PT frame, or NULL on failure.
  */
 static uint32_t* allocate_page_table_phys_buddy(void) {
     void* pt_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!pt_ptr) {
         terminal_write("[Paging] allocate_page_table_phys_buddy: BUDDY_ALLOC failed!\n");
         return NULL;
     }
     uintptr_t pt_phys = (uintptr_t)pt_ptr;

     if (!g_kernel_page_directory_virt)
         PAGING_PANIC("allocate_page_table_phys_buddy: Kernel PD virtual pointer not set!");

     // Map the newly allocated physical frame into kernel virtual space to zero it
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
          terminal_printf("[Paging] Failed to map new PT 0x%x for zeroing!\n", pt_phys);
          BUDDY_FREE(pt_ptr, PAGE_SIZE);
          return NULL;
     }
     // Zero the page table using its temporary virtual address
     memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);

     // Unmap the temporary mapping
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT); // Invalidate TLB for the temp address

     return (uint32_t*)pt_phys; // Return the physical address
 }

 /**
  * @brief Internal helper to map a single page.
  *
  * This function maps a 4KB or 4MB page into the given page directory.
  * It uses temporary virtual mappings (via TEMP_MAP_ADDR_PD and TEMP_MAP_ADDR_PT)
  * to access and update the PD and PT entries.
  *
  * @param page_directory_phys Physical address of the page directory.
  * @param vaddr Virtual address to map.
  * @param paddr Physical address of the page frame.
  * @param flags Flags for page entry (e.g. PAGE_PRESENT|PAGE_RW|PAGE_USER).
  * @param use_large_page True to attempt a 4MB mapping.
  * @return 0 on success, negative value on failure.
  */
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page) {
     if (!page_directory_phys) return -1;
     // We need the KERNEL's virtual PD pointer to map the TARGET PD/PT temporarily
     if (!g_kernel_page_directory_virt) {
         terminal_write("[Paging] map_page_internal: Kernel PD virtual pointer not set.\n");
         return -1;
     }

     uint32_t pd_idx = PDE_INDEX(vaddr);
     uintptr_t original_vaddr = vaddr;  // Keep original for TLB invalidation

     // Align addresses and set or clear the large page flag accordingly.
     if (use_large_page) {
         if (!g_pse_supported) return -1; // Cannot use large pages if not supported
         vaddr = PAGE_LARGE_ALIGN_DOWN(vaddr);
         paddr = PAGE_LARGE_ALIGN_DOWN(paddr);
         flags |= PAGE_SIZE_4MB;
     } else {
         vaddr = PAGE_ALIGN_DOWN(vaddr);
         paddr = PAGE_ALIGN_DOWN(paddr);
         flags &= ~PAGE_SIZE_4MB;
     }

     uint32_t* target_pd_virt = NULL;
     uint32_t* page_table_virt = NULL;
     uintptr_t pt_phys = 0;
     int ret = -1; // Default to error
     bool pt_allocated_here = false;

     // --- Map the TARGET page directory temporarily using the KERNEL PD ---
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0){
         terminal_write("[Paging] map_page_internal: Failed to temp map target PD.\n");
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = target_pd_virt[pd_idx]; // Read the PDE entry from the target PD

     // --- Handle Large Page Mapping ---
     if (use_large_page) {
         if (pde & PAGE_PRESENT) {
             // Cannot overwrite an existing entry (could be 4KB PT or another 4MB page)
             terminal_printf("[Paging] map_page_internal: Conflict mapping 4MB page over existing PDE 0x%x for V=0x%x\n", pde, vaddr);
             goto cleanup_map_page_pd; // Error exit, cleans up PD mapping
         }
         // Write the new 4MB PDE entry into the *target* PD (via its temp mapping)
         target_pd_virt[pd_idx] = (paddr & 0xFFC00000) | (flags & 0xFFF) | PAGE_SIZE_4MB | PAGE_PRESENT;
         ret = 0; // Success
         // Invalidate TLB for the affected 4MB range
         tlb_flush_range((void*)vaddr, PAGE_SIZE_LARGE);
         goto cleanup_map_page_pd; // Cleanup and return
     }

     // --- Handle 4KB Page Mapping ---
     // Check if PDE points to a 4MB page - cannot map 4KB page over it
     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)){
         terminal_printf("[Paging] map_page_internal: Conflict mapping 4KB page over existing 4MB PDE 0x%x for V=0x%x\n", pde, vaddr);
         goto cleanup_map_page_pd;
     }

     // Check if Page Table exists, allocate if needed
     if (!(pde & PAGE_PRESENT)) {
         pt_phys = (uintptr_t)allocate_page_table_phys_buddy(); // Uses buddy allocator now
         if (pt_phys == 0){
             terminal_write("[Paging] map_page_internal: Failed to allocate PT frame via buddy.\n");
             goto cleanup_map_page_pd;
         }
         pt_allocated_here = true;
         // Determine flags for the new PDE entry
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Default: User RW access
         if (!(flags & PAGE_USER)) // If the 4KB page is kernel-only...
             pde_flags &= ~PAGE_USER; // ...make the PT kernel-only too
         // Update the *target* PD's entry
         target_pd_virt[pd_idx] = (pt_phys & ~0xFFF) | pde_flags;
         pde = target_pd_virt[pd_idx]; // Read back the updated PDE
         // Invalidate TLB for the address (since PDE changed)
         paging_invalidate_page((void*)original_vaddr);
     } else {
         // Page Table already exists, get its physical address
         pt_phys = (uintptr_t)(pde & ~0xFFF);
         // Ensure PDE flags grant at least the access the PTE flags require
         uint32_t needed_pde_flags = (flags & (PAGE_USER | PAGE_RW));
         if ((pde & needed_pde_flags) != needed_pde_flags) {
             target_pd_virt[pd_idx] |= needed_pde_flags; // Promote PDE flags
             paging_invalidate_page((void*)original_vaddr); // Invalidate TLB
         }
     }

     // --- Map the TARGET page table temporarily using KERNEL PD ---
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         terminal_printf("[Paging] map_page_internal: Failed to temp map target PT 0x%x.\n", pt_phys);
         if (pt_allocated_here) {
             // If we just allocated this PT, free it as we can't use it
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
             target_pd_virt[pd_idx] = 0; // Clear the PDE entry pointing to it
             paging_invalidate_page((void*)original_vaddr);
         }
         goto cleanup_map_page_pd; // Error exit
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT; // Virtual address of target PT

     // --- Set the PTE entry in the TARGET page table ---
     uint32_t pt_idx = PTE_INDEX(vaddr);
     if (page_table_virt[pt_idx] & PAGE_PRESENT){
         // Cannot overwrite an existing 4KB mapping
         terminal_printf("[Paging] map_page_internal: Conflict mapping 4KB page over existing PTE 0x%x for V=0x%x\n", page_table_virt[pt_idx], vaddr);
         goto cleanup_map_page_pt; // Error exit, cleans up PT and PD mappings
     }
     page_table_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     ret = 0; // Success

 cleanup_map_page_pt:
     // Unmap the temporary mapping of the TARGET PT
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT); // Invalidate TLB for the temp addr

 cleanup_map_page_pd:
     // Unmap the temporary mapping of the TARGET PD
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD); // Invalidate TLB for the temp addr

     // If mapping succeeded, invalidate TLB for the actual virtual address mapped
     if (ret == 0)
         paging_invalidate_page((void*)original_vaddr);

     return ret;
 }

 /**
  * @brief Public API: Map a single 4KB page.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param vaddr Virtual address.
  * @param paddr Physical frame address.
  * @param flags Page entry flags.
  * @return 0 on success, negative on error.
  */
 int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
     flags |= PAGE_PRESENT; // Ensure the present flag is set.
     return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
 }

 /**
  * @brief Map a range of pages, using large pages where possible.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param virt_start_addr Starting virtual address.
  * @param phys_start_addr Starting physical address.
  * @param memsz Size (in bytes) to map.
  * @param flags Flags to use.
  * @return 0 on success, negative on error.
  */
 int paging_map_range(uint32_t *page_directory_phys, uint32_t virt_start_addr,
                      uint32_t phys_start_addr, uint32_t memsz, uint32_t flags)
 {
     if (!page_directory_phys || memsz == 0)
         return -1;
     flags |= PAGE_PRESENT; // Ensure present flag is set
     uintptr_t v = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t p = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t end = PAGE_ALIGN_UP(virt_start_addr + memsz); // Exclusive end

     while (v < end) {
         // Check if a 4MB page can be used here
         bool can_use_large = g_pse_supported &&
                              (v % PAGE_SIZE_LARGE == 0) && // Virtual address aligned?
                              (p % PAGE_SIZE_LARGE == 0) && // Physical address aligned?
                              ((end - v) >= PAGE_SIZE_LARGE); // Enough space left?

         size_t step = can_use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;

         if (map_page_internal(page_directory_phys, v, p, flags, can_use_large) != 0) {
             terminal_printf("paging_map_range: Failed mapping page V=0x%x -> P=0x%x (Large=%d)\n", v, p, can_use_large);
             // Consider unmapping already mapped pages in this range on failure?
             return -1;
         }
         v += step;
         p += step;
     }
     return 0; // Success
 }

 /**
  * @brief Check if a page table is empty (i.e. no entry is marked PRESENT).
  * @param pt_virt The virtual address of the page table (mapped temporarily).
  * @return true if empty, false otherwise.
  */
 static bool is_page_table_empty(uint32_t* pt_virt) {
     if (!pt_virt)
         return true; // NULL pointer is considered empty
     for (int i = 0; i < 1024; ++i) {
         if (pt_virt[i] & PAGE_PRESENT)
             return false; // Found a present entry
     }
     return true; // No present entries found
 }

 /**
  * @brief Unmap a range of virtual addresses.
  * Releases the physical frames (via put_frame) and page tables (via BUDDY_FREE)
  * associated with the mapping.
  *
  * @param page_directory_phys Physical address of the PD to modify.
  * @param virt_start_addr Starting virtual address.
  * @param memsz Size (in bytes) of the range to unmap.
  * @return 0 on success, negative on error.
  */
 int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz) {
     if (!page_directory_phys || memsz == 0)
         return -1;
     // Need kernel virtual PD to perform temporary mappings
     if (!g_kernel_page_directory_virt)
         return -1;

     uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t virt_end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
     if (virt_end <= virt_start)
         return -1; // Invalid range

     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;
     int final_result = 0; // Track if any errors occurred

     // --- Map the TARGET PD temporarily ---
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0){
         terminal_write("[Paging] unmap_range: Failed to temp map target PD.\n");
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;

     // --- Iterate through the virtual address range ---
     for (uintptr_t v = virt_start; v < virt_end; /* v incremented inside */ ) {
         uint32_t pd_idx = PDE_INDEX(v);
         uint32_t pde = target_pd_virt[pd_idx];

         // If PDE is not present, skip this 4MB range
         if (!(pde & PAGE_PRESENT)) {
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Align to next PDE boundary
             continue;
         }

         // --- Handle 4MB Page Unmapping ---
         if (pde & PAGE_SIZE_4MB) {
             uintptr_t large_page_v_start = PAGE_LARGE_ALIGN_DOWN(v);
             // Check if the ENTIRE 4MB page falls within the requested unmap range
             if (large_page_v_start >= virt_start && (large_page_v_start + PAGE_SIZE_LARGE) <= virt_end) {
                 uintptr_t frame_base_phys = pde & 0xFFC00000;
                 // Release all constituent 4KB frames within the 4MB region.
                 for (int i = 0; i < 1024; ++i) {
                     put_frame(frame_base_phys + i * PAGE_SIZE); // Use frame allocator's put
                 }
                 target_pd_virt[pd_idx] = 0; // Clear the PDE
                 tlb_flush_range((void*)large_page_v_start, PAGE_SIZE_LARGE); // Flush TLB for the whole 4MB range
             } else {
                 // Partial unmap of a 4MB page is complex and usually avoided.
                 // For simplicity, report error or ignore partial unmap request for 4MB pages.
                 terminal_printf("[Paging] unmap_range: Warning - Partial unmap of 4MB page at V=0x%x requested. Skipping.\n", large_page_v_start);
                 final_result = -1; // Indicate potential issue
             }
             v = large_page_v_start + PAGE_SIZE_LARGE; // Move to the next 4MB boundary
             continue;
         }

         // --- Handle 4KB Page Unmapping ---
         uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF);
         pt_virt = NULL;
         bool pt_was_freed = false;

         // Map the target Page Table temporarily
         if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys_val, PTE_KERNEL_DATA_FLAGS) == 0) {
             pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;

             // Calculate the end address for this specific PT's range
             uintptr_t pt_range_v_end = PAGE_ALIGN_DOWN(v) + PAGE_SIZE_LARGE; // End of 4MB range covered by this PT
             uintptr_t loop_end = (virt_end < pt_range_v_end) ? virt_end : pt_range_v_end; // Don't go past requested end

             // Iterate through PTEs within the current 4MB range or requested range
             while (v < loop_end) {
                 uint32_t pt_idx = PTE_INDEX(v);
                 uint32_t pte = pt_virt[pt_idx];

                 if (pte & PAGE_PRESENT) {
                     uintptr_t frame_phys = pte & ~0xFFF;
                     put_frame(frame_phys); // Free the physical frame using frame allocator
                     pt_virt[pt_idx] = 0;   // Clear the PTE
                     paging_invalidate_page((void*)v); // Invalidate TLB for this specific page
                 }
                 v += PAGE_SIZE; // Move to the next page
             }

             // Check if the page table is now empty
             if (is_page_table_empty(pt_virt)) {
                 target_pd_virt[pd_idx] = 0; // Clear the PDE entry
                 // Invalidate TLB for addresses covered by this PDE (can be broad)
                 // A single invalidation might suffice if the entry is just cleared
                 paging_invalidate_page((void*)PAGE_ALIGN_DOWN(v - PAGE_SIZE)); // Invalidate last address in range

                 // Unmap the temporary PT mapping BEFORE freeing its physical frame
                 paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                 paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
                 pt_virt = NULL; // Mark as unmapped
                 pt_was_freed = true;

                 // Free the physical frame used by the page table itself
                 BUDDY_FREE((void*)pt_phys_val, PAGE_SIZE);
             }

             // If PT wasn't freed, still need to unmap its temporary mapping
             if (pt_virt && !pt_was_freed) {
                 paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                 paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
             }
         } else {
             // Failed to map the PT, cannot proceed with this range
             terminal_printf("[Paging] unmap_range: Failed to temp map PT 0x%x.\n", pt_phys_val);
             final_result = -1; // Mark error
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip to next PDE
         }
     } // End for loop over virtual address range

     // --- Unmap the TARGET PD ---
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

     return final_result;
 }

 /**
  * @brief Identity map a range (virt_addr == phys_addr) with given flags.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param start_addr Starting address to map.
  * @param size Size (in bytes) of the range.
  * @param flags Flags for the mapping.
  * @return 0 on success, negative on error.
  */
 int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, uint32_t size, uint32_t flags) {
     return paging_map_range(page_directory_phys, start_addr, start_addr, size, flags | PAGE_PRESENT);
 }

 /**
  * @brief Page Fault Handler.
  *
  * This function is called on a page fault. It prints fault information,
  * attempts to handle faults via the virtual memory subsystem (find_vma, handle_vma_fault),
  * and, if unhandled, terminates the offending process.
  *
  * @param regs Pointer to the registers (including error code and EIP).
  */
 void page_fault_handler(registers_t *regs) {
     uint32_t fault_addr;
     asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
     uint32_t error_code = regs->err_code;
     bool present = (error_code & 0x1) != 0;
     bool write = (error_code & 0x2) != 0;
     bool user = (error_code & 0x4) != 0;
     bool reserved_bit = (error_code & 0x8) != 0;
     bool instruction_fetch = (error_code & 0x10) != 0;
     pcb_t* current_process = get_current_process();
     uint32_t current_pid = current_process ? current_process->pid : 0;

     terminal_printf("\n--- PAGE FAULT (PID %d) ---\n", current_pid);
     terminal_printf(" Addr: 0x%x Code: 0x%x (%s %s %s %s %s)\n", fault_addr, error_code,
                     present ? "P" : "NP",
                     write ? "W" : "R",
                     user ? "U" : "S",
                     reserved_bit ? "RSV" : "-",
                     instruction_fetch ? "IF" : "-");
     terminal_printf(" EIP: 0x%x\n", regs->eip);

     if (reserved_bit) {
         terminal_write("  Error: Reserved bit violation!\n");
         goto unhandled_fault;
     }
     // Strict check: Kernel should NEVER fault accessing addresses below KERNEL_SPACE_VIRT_START
     if (!user && fault_addr < KERNEL_SPACE_VIRT_START) {
         terminal_write("  Error: Kernel fault accessing address below kernel base!\n");
         goto unhandled_fault;
     }
     // Check if paging structures are ready
     if (!g_kernel_page_directory_virt) {
         terminal_write("  Error: Page fault occurred before kernel PD virtual address was set!\n");
         goto unhandled_fault_early;
     }
     // Check if in valid process context
     if (!current_process || !current_process->mm) {
         // This can happen if the fault occurs very early or in an interrupt context without a process
         terminal_write("  Error: Page fault outside valid process context!\n");
         if (!user) { // If it was a kernel fault outside a process, it's likely fatal
             goto unhandled_fault_early;
         } else { // User fault outside a process? Should not happen.
            goto unhandled_fault; // Treat as unhandled for now
         }
     }

     // Attempt to handle the fault using the Virtual Memory Area (VMA) system
     mm_struct_t *mm = current_process->mm;
     vma_struct_t *vma = find_vma(mm, fault_addr); // This needs to be thread-safe if using SMP

     if (!vma) {
         terminal_printf("  Error: No VMA found for addr 0x%x. Segmentation Fault.\n", fault_addr);
         goto unhandled_fault;
     }
     terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);

     // Delegate fault handling to mm.c
     int result = handle_vma_fault(mm, vma, fault_addr, error_code); // Needs to be thread-safe if SMP

     if (result == 0) {
         // Fault was handled successfully (e.g., page allocated, COW performed)
         return; // Return to the interrupted instruction
     } else {
         // Fault could not be handled by the VM system (e.g., permission error, out of memory)
         terminal_printf("  Error: handle_vma_fault failed (code %d). Segmentation Fault.\n", result);
         goto unhandled_fault;
     }

 unhandled_fault:
     // This section is reached if the fault is unrecoverable within the process context
     terminal_write("--- Unhandled Page Fault ---\n");
     terminal_printf(" Terminating process (PID %d) due to page fault at 0x%x.\n", current_pid, fault_addr);
     if (user) { // Print user stack info if available
         terminal_printf(" UserESP: 0x%x UserSS: 0x%x\n", regs->user_esp, regs->user_ss);
     }
     terminal_printf("--------------------------\n");
     uint32_t exit_code = 0xFE000000 | error_code; // Use a distinct exit code
     remove_current_task_with_code(exit_code); // Request scheduler terminate the task
     // remove_current_task_with_code should NOT return.

 unhandled_fault_early:
     // This section is reached for critical kernel faults or faults before scheduler/process init
     terminal_write("--- Unhandled Page Fault (Kernel/Early Init Stage) ---\n");
     terminal_write("Cannot recover. System Halted.\n");
     PAGING_PANIC("Early or Kernel Page Fault"); // Use panic macro to halt
 }

 /**
  * @brief Free user-space mappings and associated Page Tables.
  *
  * Iterates through the user-space portion of the Page Directory.
  * If a PDE points to a Page Table, that Page Table frame is freed using BUDDY_FREE.
  * The PDE itself is then cleared. Does not free the actual data frames mapped by the PTs;
  * that should be handled by VMA cleanup (calling paging_unmap_range or put_frame).
  *
  * @param page_directory_phys Physical address of the PD to clean.
  */
  void paging_free_user_space(uint32_t *page_directory_phys) {
    if (!page_directory_phys || !g_kernel_page_directory_virt) return;
    uint32_t *target_pd_virt = NULL;

    // Map the target PD temporarily
    if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0){
         terminal_write("[Paging] free_user_space: Failed to temp map target PD.\n");
         return;
    }
    target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;

    // Iterate through user-space PDE entries (0 to KERNEL_PDE_INDEX - 1)
    for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
        uint32_t pde = target_pd_virt[i];
        // Check if PDE is present and does NOT map a 4MB page
        if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) {
            uintptr_t pt_phys = pde & ~0xFFF; // Physical address of the Page Table
            // Free the Page Table frame itself
            BUDDY_FREE((void*)pt_phys, PAGE_SIZE); // Use BUDDY_FREE
        }
        // Clear the PDE entry regardless of whether it was a PT or 4MB page (user space cleanup)
        target_pd_virt[i] = 0;
    }

    // Unmap the temporary PD mapping
    paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PD); // Invalidate TLB for temp mapping
}