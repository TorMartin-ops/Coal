/**
 * paging.c
 *
 * World-Class Paging Module for a 32-bit x86 OS.
 * Implements page directory and page table management, virtual->physical mapping.
 */

 #include "paging.h"
 #include "buddy.h"     // For allocate_page_table using buddy_alloc
 #include "terminal.h"  // For debug printing
 #include "types.h"     // Common types like uint32_t, size_t, PAGE_SIZE
 #include <string.h>    // For memset
 
 // --- Constants for Page Directory/Table Entries ---
 // Defined in paging.h: PAGE_PRESENT, PAGE_RW, PAGE_USER
 
 // --- Virtual Memory Layout ---
 // Defined in paging.h: KERNEL_SPACE_VIRT_START
 
 // --- Helper Macros ---
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF) // Page Directory Entry Index (Top 10 bits)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF) // Page Table Entry Index (Middle 10 bits)
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 
 // --- Global Pointer to the Kernel's Page Directory (Virtual Address) ---
 // This should point to the page directory structure itself, accessible via kernel virtual memory.
 uint32_t* kernel_page_directory = NULL;
 
 /**
  * @brief Sets the global kernel page directory pointer.
  * @param pd_virt Virtual address of the page directory.
  */
 void paging_set_directory(uint32_t* pd_virt) {
     kernel_page_directory = pd_virt;
 }
 
 /**
  * @brief Allocates a physical page for a page table using the buddy allocator.
  * @return Physical address of the allocated page table, or NULL on failure.
  */
 static uint32_t* allocate_page_table(void) {
     // terminal_printf("[Paging] Allocating page table, buddy free: %u bytes\n", buddy_free_space());
     uint32_t* pt_phys = (uint32_t*)buddy_alloc(PAGE_SIZE); // Buddy returns physical address
     if (!pt_phys) {
         terminal_write("[Paging] ERROR: allocate_page_table: buddy_alloc(PAGE_SIZE) failed!\n");
         return NULL;
     }
     // The caller is responsible for mapping this physical page table
     // into virtual memory (if needed) and clearing it via that mapping.
     // terminal_printf("[Paging] Page table allocated at physical addr 0x%x\n", (uintptr_t)pt_phys);
     return pt_phys; // Return physical address
 }
 
 /**
  * @brief Invalidates the TLB entry for a single virtual address.
  * @param vaddr The virtual address to invalidate.
  */
 static inline void paging_invalidate_page(void *vaddr) {
     __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }
 
 /**
  * @brief Maps a single virtual page to its identical physical address in the kernel directory.
  * (Deprecated or use with caution - prefer paging_map_range).
  * @param virt_addr The virtual (and physical) address to map.
  */
 void paging_map_page(void* virt_addr) {
      terminal_write("[Paging] WARNING: paging_map_page is likely deprecated. Use paging_map_range.\n");
      if (!kernel_page_directory) {
          terminal_write("[Paging] ERROR: paging_map_page: kernel_page_directory not set.\n");
          return;
      }
      // Use the main mapping function for consistency
      paging_map_range(kernel_page_directory, // Use global kernel PD (virtual addr)
                       (uint32_t)virt_addr,   // Virtual start
                       (uint32_t)virt_addr,   // Physical start (identity)
                       PAGE_SIZE,             // Map one page
                       PAGE_PRESENT | PAGE_RW); // Kernel flags
 }
 
 
 /**
  * @brief Maps a contiguous range of virtual addresses to physical addresses.
  * @param page_directory_virt Virtual address of the page directory to modify.
  * @param virt_start_addr     Start virtual address (will be page-aligned down).
  * @param phys_start_addr     Start physical address (will be page-aligned down).
  * @param memsz               Size of the range in bytes.
  * @param flags               Flags for the PTEs (e.g., PAGE_PRESENT | PAGE_RW | PAGE_USER).
  * @return 0 on success, -1 on failure.
  */
 int paging_map_range(uint32_t *page_directory_virt, uint32_t virt_start_addr,
                      uint32_t phys_start_addr, uint32_t memsz, uint32_t flags)
 {
     if (!page_directory_virt) {
         terminal_write("[Paging] ERROR: map_range: page_directory_virt is NULL!\n");
         return -1;
     }
     if (memsz == 0) {
         return 0; // Nothing to map
     }
 
     // Align addresses and calculate end address
     uintptr_t virt_aligned_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t phys_aligned_start = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t virt_aligned_end = PAGE_ALIGN_UP(virt_start_addr + memsz);
 
     // Basic check for address wrapping
     if (virt_aligned_end <= virt_aligned_start) {
         terminal_printf("[Paging] ERROR: map_range: Potential virtual address wrap around or invalid size.\n");
         return -1;
     }
 
     // terminal_printf("[Paging] map_range: V:0x%x -> P:0x%x (Size: %u KB, Flags: 0x%x)\n",
     //                virt_aligned_start, phys_aligned_start,
     //                (virt_aligned_end - virt_aligned_start) / 1024, flags);
 
     uintptr_t current_phys = phys_aligned_start;
     for (uintptr_t current_virt = virt_aligned_start; current_virt < virt_aligned_end;
          current_virt += PAGE_SIZE, current_phys += PAGE_SIZE)
     {
         uint32_t pd_idx = PDE_INDEX(current_virt);
         uint32_t pt_idx = PTE_INDEX(current_virt);
 
         // Ensure PD index is valid
         if (pd_idx >= 1024) {
             terminal_printf("[Paging] ERROR: map_range: Invalid PDE index %d for vaddr 0x%x\n", pd_idx, current_virt);
             return -1; // Should not happen with 32-bit addresses
         }
 
         uint32_t pd_entry = page_directory_virt[pd_idx];
         uint32_t* page_table_phys; // Physical address of the page table
         uint32_t* page_table_virt; // Virtual address to access the PT contents
 
         if (!(pd_entry & PAGE_PRESENT)) {
             // --- Page Directory Entry Not Present: Allocate Page Table ---
             page_table_phys = allocate_page_table(); // Get physical page for PT
             if (!page_table_phys) {
                 terminal_write("[Paging] ERROR: map_range: Failed to allocate page table!\n");
                 // Should potentially unmap successfully mapped pages in this range? Complex.
                 return -1; // Allocation failed
             }
 
             // Map the physical PT page into kernel virtual space to clear/access it.
             // ASSUMPTION: The kernel's higher-half mapping covers the buddy heap area.
             page_table_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)page_table_phys);
 
             // Clear the new page table via its virtual address
             memset(page_table_virt, 0, PAGE_SIZE);
 
             // Update PDE: Point to physical PT. Propagate USER/RW flags to PDE
             // to ensure the table itself is accessible appropriately.
             uint32_t pde_flags = PAGE_PRESENT | (flags & (PAGE_USER | PAGE_RW));
             page_directory_virt[pd_idx] = (uint32_t)page_table_phys | pde_flags;
 
             // Invalidate TLB for the PDE? Not strictly necessary, CPU handles it on PT walk?
             // No harm in invalidating the first address that would use this PDE.
             paging_invalidate_page((void*)current_virt);
 
         } else {
             // --- Page Directory Entry IS Present: Use Existing Page Table ---
             page_table_phys = (uint32_t*)(pd_entry & ~0xFFF); // Get physical address of PT
             // Calculate virtual address to access the existing PT
             page_table_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + (uintptr_t)page_table_phys);
 
             // Ensure PDE permissions are sufficient if mapping with USER/RW flags
             page_directory_virt[pd_idx] |= (flags & (PAGE_USER | PAGE_RW)) | PAGE_PRESENT;
         }
 
         // --- Set Page Table Entry ---
         // Ensure PT index is valid (sanity check)
         if (pt_idx >= 1024) {
              terminal_printf("[Paging] ERROR: map_range: Invalid PTE index %d for vaddr 0x%x\n", pt_idx, current_virt);
              // Potential corruption if we write here
              return -1;
         }
 
         // Check if PTE already exists - optional warning/policy
         // if (page_table_virt[pt_idx] & PAGE_PRESENT) {
         //     terminal_printf("[Paging] WARN: map_range: Overwriting existing PTE for vaddr 0x%x\n", current_virt);
         // }
 
         // Set the PTE: Physical target page address + flags
         page_table_virt[pt_idx] = (current_phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
 
         // Invalidate TLB entry for the specific virtual address being mapped/updated
         paging_invalidate_page((void*)current_virt);
     }
     return 0; // Success
 }
 
 
 /**
  * @brief Helper function to create identity mapping for a range [0..size).
  * @param page_directory_virt Virtual address of the page directory to modify.
  * @param size The upper bound (exclusive) of the range to map.
  * @param flags Flags to apply (e.g., PAGE_PRESENT | PAGE_RW).
  * @return 0 on success, -1 on failure.
  */
 int paging_init_identity_map(uint32_t *page_directory_virt, uint32_t size, uint32_t flags) {
     terminal_printf("[Paging] Creating identity map [0x0 - 0x%x)\n", size);
     // Call the main mapping function with virtual start = 0, physical start = 0
     return paging_map_range(page_directory_virt, 0, 0, size, flags);
 }
 
 
 /**
  * @brief Activates a given page directory and enables paging.
  * @param page_directory_phys Physical address pointer to the page directory.
  */
 void paging_activate(uint32_t *page_directory_phys) {
     // Load physical address of page directory into CR3
     __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory_phys) : "memory");
 
     // Enable paging (PG bit, bit 31) in CR0
     uint32_t cr0;
     __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit
     __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
 
      terminal_write("[Paging] Paging enabled via CR0 and CR3 loaded.\n");
 }