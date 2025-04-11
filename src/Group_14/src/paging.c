/**
 * paging.c - Paging Implementation with PSE (4MB Pages) support.
 */

 #include "paging.h"
 #include "frame.h"          // Still needed for put_frame (used AFTER frame_init)
 #include "buddy.h"          // <<< Need buddy_alloc/buddy_free >>>
 #include "terminal.h"
 #include "types.h"
 #include "process.h"        // Included for page_fault_handler context (optional)
 #include "mm.h"             // Included for page_fault_handler context (optional)
 #include "scheduler.h"      // Included for page_fault_handler context (optional)
 #include <string.h>         // For memset
 #include "cpuid.h"          // For CPUID instruction helper
 #include "kmalloc_internal.h" // For ALIGN_UP
 
 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL; // Virtual address pointer (set after mapping)
 uint32_t g_kernel_page_directory_phys = 0;    // Physical address (set during init)
 bool g_pse_supported = false;                  // PSE support flag
 
 // Temporary virtual addresses for mapping PD/PT during operations
 // These MUST be in the higher-half kernel space and available *after* paging is active
 #ifndef TEMP_MAP_ADDR_PT
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PD
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
 #endif
 
 
 // --- Helper Macros ---
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
 // PAGE_ALIGN_DOWN/UP provided by kmalloc_internal.h or types.h potentially
 #ifndef PAGE_ALIGN_DOWN
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #endif
 #ifndef PAGE_ALIGN_UP
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 #endif
 #define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
 #define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))
 
 
 // --- Forward Declarations ---
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static uint32_t* allocate_page_table_phys_buddy(void); // Helper using buddy
 
 
 // --- CPU Feature Detection and Control ---
 static inline uint32_t read_cr4(void) {
    uint32_t val;
    asm volatile("mov %%cr4, %0" : "=r"(val));
    return val;
 }
 static inline void write_cr4(uint32_t value) {
    asm volatile("mov %0, %%cr4" : : "r"(value));
 }
 static inline void enable_cr4_pse(void) {
    write_cr4(read_cr4() | (1 << 4)); // Set PSE bit (bit 4)
 }
 bool check_and_enable_pse(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx); // Use cpuid helper
 
    if (edx & (1 << 3)) { // Check EDX bit 3 for PSE support
        terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
        enable_cr4_pse();
        uint32_t cr4_val = read_cr4();
        if(cr4_val & (1 << 4)) {
             terminal_write("[Paging] CR4.PSE bit enabled.\n");
             g_pse_supported = true;
             return true;
        } else {
             terminal_write("[Paging] Error: Failed to enable CR4.PSE bit!\n");
             g_pse_supported = false;
             return false;
        }
    } else {
        terminal_write("[Paging] CPU does not support PSE (4MB Pages).\n");
        g_pse_supported = false;
        return false;
    }
 }
 
 
 // --- Public API Implementation ---
 
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }
 
 void paging_invalidate_page(void *vaddr) {
     asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }
 
 void tlb_flush_range(void* start, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr = ALIGN_UP((uintptr_t)start + size, PAGE_SIZE);
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr);
         addr += PAGE_SIZE;
     }
 }
 
 void paging_activate(uint32_t *page_directory_phys) {
     uint32_t cr0;
     asm volatile("mov %0, %%cr3" : : "r"(page_directory_phys) : "memory"); // Load physical address into CR3
     asm volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit (Enable Paging)
     asm volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 
 // Helper function to allocate and zero a Page Table frame using BUDDY
 // This should only be called AFTER paging is active, so virtual mapping works.
 static uint32_t* allocate_page_table_phys_buddy(void) {
     void* pt_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!pt_ptr) {
         terminal_write("[Paging] allocate_page_table_phys_buddy: BUDDY FAILED!\n");
         return NULL;
     }
     uintptr_t pt_phys = (uintptr_t)pt_ptr;
 
     // Map temporarily to zero it using the active kernel page directory
     if (!g_kernel_page_directory_phys) {
         terminal_write("[Paging] allocate_page_table_phys_buddy: Kernel PD not set for mapping!\n");
         BUDDY_FREE(pt_ptr, PAGE_SIZE);
         return NULL;
     }
     if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA) != 0) {
          terminal_printf("[Paging] Failed to map new PT 0x%x for zeroing!\n", pt_phys);
          BUDDY_FREE(pt_ptr, PAGE_SIZE);
          return NULL;
     }
     memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
 
     return (uint32_t*)pt_phys; // Return physical address
 }
 
 
  /**
   * @brief Internal helper to map a single page (4KB or 4MB).
   * Manages temporary mappings of PD and PT. Uses buddy allocator for PTs.
   * PRECONDITION: Paging must be active for temporary mappings to work reliably.
   */
  static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page) {
      if (!page_directory_phys) return -1;
      if (!g_kernel_page_directory_phys) {
          terminal_write("[Paging] map_page_internal: Cannot map, kernel PD not set.\n");
          return -1; // Cannot perform temporary mappings
      }
 
      uint32_t pd_idx = PDE_INDEX(vaddr);
      uintptr_t original_vaddr = vaddr; // Keep original for invalidation
 
      // Align addresses based on page size requested
      if (use_large_page) {
          if (!g_pse_supported) {
              terminal_printf("map_page_internal: Error - Attempted large page map but PSE not supported/enabled.\n");
              return -1;
          }
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
      uintptr_t pt_phys = 0; // Use uintptr_t for physical address
      int ret = -1;
      bool pt_allocated_here = false; // Track if we allocated PT in this call
 
      // Map target PD temporarily using the *active* kernel PD
      if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA) != 0) {
          terminal_write("map_page_internal: Failed temp map target PD\n");
          return -1;
      }
      target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
      uint32_t pde = target_pd_virt[pd_idx];
 
      // --- Handle Large Page Mapping ---
      if (use_large_page) {
          if (pde & PAGE_PRESENT) {
              if (!(pde & PAGE_SIZE_4MB)) {
                  terminal_printf("map_page_internal: Error - Cannot map 4MB page, PDE %d already points to a Page Table.\n", pd_idx);
              } else {
                  terminal_printf("map_page_internal: Error - PDE %d already maps a 4MB page.\n", pd_idx);
              }
              goto cleanup_map_page_pd; // Error
          }
          target_pd_virt[pd_idx] = (paddr & 0xFFC00000) | (flags & 0xFFF) | PAGE_SIZE_4MB | PAGE_PRESENT;
          ret = 0; // Success
          tlb_flush_range((void*)vaddr, PAGE_SIZE_LARGE);
          goto cleanup_map_page_pd; // Skip PT handling
      }
 
      // --- Handle 4KB Page Mapping ---
      if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)) {
          terminal_printf("map_page_internal: Error - Cannot map 4KB page, PDE %d already maps a 4MB page.\n", pd_idx);
          goto cleanup_map_page_pd;
      }
 
      // Ensure Page Table Exists
      if (!(pde & PAGE_PRESENT)) {
          // Use buddy helper to allocate PT
          pt_phys = (uintptr_t)allocate_page_table_phys_buddy(); // Get phys addr
          if (pt_phys == 0) {
              terminal_write("[Paging] map_page_internal: Failed to allocate PT frame via buddy.\n");
              goto cleanup_map_page_pd; // Allocation failed
          }
          pt_allocated_here = true;
          uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Default flags
          if (!(flags & PAGE_USER)) { pde_flags &= ~PAGE_USER; } // Inherit USER status
          target_pd_virt[pd_idx] = (pt_phys & ~0xFFF) | pde_flags;
          pde = target_pd_virt[pd_idx]; // Update local copy
          paging_invalidate_page((void*)original_vaddr); // Invalidate for changed PDE range
 
      } else {
          pt_phys = (uintptr_t)(pde & ~0xFFF); // Get existing PT physical address
          // Update PDE flags if necessary
          uint32_t needed_pde_flags = (flags & (PAGE_USER | PAGE_RW));
          if ((pde & needed_pde_flags) != needed_pde_flags) {
              target_pd_virt[pd_idx] |= needed_pde_flags;
              paging_invalidate_page((void*)original_vaddr); // Invalidate for changed PDE flags
          }
      }
 
      // Map the Page Table temporarily using the *active* kernel PD
      if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA) != 0) {
          terminal_printf("[Paging] map_page_internal: Failed temp map PT (Phys=0x%x)\n", pt_phys);
          // If we allocated the PT in this call, free it using BUDDY
          if (pt_allocated_here) {
               BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
               target_pd_virt[pd_idx] = 0; // Clear the PDE we just set
               paging_invalidate_page((void*)original_vaddr);
          }
          goto cleanup_map_page_pd;
      }
      page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
      uint32_t pt_idx = PTE_INDEX(vaddr); // Use aligned vaddr
 
      // Check if PTE already exists
      if (page_table_virt[pt_idx] & PAGE_PRESENT) {
           terminal_printf("map_page_internal: Error - PTE already present for vaddr 0x%x!\n", vaddr);
           // Don't free PT frame if it pre-existed
           goto cleanup_map_page_pt; // Error
      }
 
      // Set the Page Table Entry
      page_table_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
      ret = 0; // Success
 
  cleanup_map_page_pt:
      // Unmap temp PT mapping
      paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
      paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
 
  cleanup_map_page_pd:
      // Unmap temp PD mapping
      paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
      paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
 
      if (ret == 0) {
          paging_invalidate_page((void*)original_vaddr); // Invalidate specific page TLB on success
      }
 
      return ret;
  }
 
 
  // Public function: Maps a single 4KB page
  int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
      return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
  }
 
  // Maps a range, potentially using large pages if aligned and supported
  int paging_map_range(uint32_t *page_directory_phys, uint32_t virt_start_addr,
                       uint32_t phys_start_addr, uint32_t memsz, uint32_t flags)
  {
      if (!page_directory_phys || memsz == 0) { return -1; }
 
      uintptr_t v = PAGE_ALIGN_DOWN(virt_start_addr);
      uintptr_t p = PAGE_ALIGN_DOWN(phys_start_addr);
      uintptr_t end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
 
      while (v < end) {
          bool can_use_large = g_pse_supported &&
                               (v % PAGE_SIZE_LARGE == 0) &&
                               (p % PAGE_SIZE_LARGE == 0) &&
                               ((end - v) >= PAGE_SIZE_LARGE);
 
          size_t step = can_use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
          if (map_page_internal(page_directory_phys, v, p, flags, can_use_large) != 0) {
              terminal_printf("paging_map_range: Failed mapping page V=0x%x -> P=0x%x (Large=%d)\n", v, p, can_use_large);
              // TODO: Implement rollback for previously mapped pages in this range?
              return -1;
          }
          v += step;
          p += step;
      }
      return 0;
  }
 
  // Helper to check if PT is empty
  static bool is_page_table_empty(uint32_t* pt_virt) {
      if (!pt_virt) return true;
      for (int i = 0; i < 1024; ++i) {
          if (pt_virt[i] & PAGE_PRESENT) {
              return false;
          }
      }
      return true;
  }
 
 
  /**
   * @brief Unmaps a range, handling both 4KB and 4MB pages.
   * Frees page tables using BUDDY_FREE if they become empty.
   * Frees physical frames using put_frame (requires frame allocator).
   */
   int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz) {
     if (!page_directory_phys || memsz == 0) return -1;
     if (!g_kernel_page_directory_phys) return -1; // Need kernel PD to map target PD/PTs
 
     uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t virt_end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
     if (virt_end <= virt_start) return -1;
 
     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;
     int final_result = 0; // Track overall success/failure
 
     // Map target PD temporarily
     if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA) != 0) {
         terminal_write("[Paging] unmap_range: Failed map target PD.\n");
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
 
     for (uintptr_t v = virt_start; v < virt_end; ) {
           uint32_t pd_idx = PDE_INDEX(v);
           // Ensure index is within valid range (especially below kernel space)
           if (page_directory_phys != (uint32_t*)g_kernel_page_directory_phys && pd_idx >= KERNEL_PDE_INDEX) {
                v = (pd_idx + 1) * PAGE_SIZE_LARGE; // Skip kernel PDEs if not unmapping kernel itself
                continue;
           }
           uint32_t pde = target_pd_virt[pd_idx];
 
           if (!(pde & PAGE_PRESENT)) {
               v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip to next PDE if not present
               continue;
           }
 
           // --- Handle 4MB Page Unmapping ---
           if (pde & PAGE_SIZE_4MB) {
                uintptr_t large_page_v_start = PAGE_ALIGN_DOWN(v);
                // Only unmap if the entire 4MB page falls within the requested range
                if (large_page_v_start >= virt_start && (large_page_v_start + PAGE_SIZE_LARGE) <= virt_end) {
                     uintptr_t frame_base_phys = pde & 0xFFC00000;
                     terminal_printf("  [Unmap] Releasing 4MB Page: V=0x%x (PDE %d)\n", large_page_v_start, pd_idx);
                     // Use put_frame (assumes frame allocator is initialized)
                     for (int i = 0; i < 1024; ++i) { put_frame(frame_base_phys + i * PAGE_SIZE); }
                     target_pd_virt[pd_idx] = 0; // Clear PDE
                     tlb_flush_range((void*)large_page_v_start, PAGE_SIZE_LARGE); // Flush entire 4MB range
                } else {
                     // Cannot partially unmap a 4MB page easily. Log error or potentially break it down (complex).
                     terminal_printf("  [Unmap Error] Cannot partially unmap 4MB page (V=0x%x, PDE %d). Range [0x%x-0x%x)\n", v, pd_idx, virt_start, virt_end);
                     final_result = -1; // Indicate error, but continue checking other pages
                }
                v = large_page_v_start + PAGE_SIZE_LARGE; // Advance by 4MB regardless of success/failure for this PDE
                continue; // Move to next iteration
           }
 
           // --- Handle 4KB Page Unmapping (within a Page Table) ---
           uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF); // Physical address of PT
           pt_virt = NULL;
           bool pt_was_freed = false;
 
           // Map PT temporarily
           if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, pt_phys_val, PTE_KERNEL_DATA) == 0) {
                pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
                // Calculate the end of the virtual range covered by this PT
                uintptr_t pt_range_v_end = PAGE_ALIGN_DOWN(v) + PAGE_SIZE_LARGE;
                // Clamp the loop end to the overall unmap range end or the end of this PT's coverage
                uintptr_t loop_end = (virt_end < pt_range_v_end) ? virt_end : pt_range_v_end;
 
                while (v < loop_end) {
                     uint32_t pt_idx = PTE_INDEX(v);
                     uint32_t pte = pt_virt[pt_idx];
                     if (pte & PAGE_PRESENT) {
                          uintptr_t frame_phys = pte & ~0xFFF;
                          put_frame(frame_phys); // Use frame allocator to decrease refcount/free
                          pt_virt[pt_idx] = 0;   // Clear PTE
                          paging_invalidate_page((void*)v); // Invalidate TLB for this specific page
                     }
                     v += PAGE_SIZE; // Move to the next page
                } // End PTE loop for this PT
 
                // Check if PT became empty after unmapping pages
                if (is_page_table_empty(pt_virt)) {
                     target_pd_virt[pd_idx] = 0; // Clear PDE
                     // Invalidate range covered by the PDE (optional but safer)
                     // tlb_flush_range((void*)PAGE_ALIGN_DOWN(v - PAGE_SIZE), PAGE_SIZE_LARGE);
 
                     // Unmap temp PT mapping *before* freeing the PT frame
                     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
                     pt_virt = NULL; // Mark as unmapped
                     pt_was_freed = true;
 
                     // Free the PT frame itself using BUDDY
                     BUDDY_FREE((void*)pt_phys_val, PAGE_SIZE);
                     // terminal_printf("  [Unmap] Freed empty page table (Phys=0x%x) for PDE %d\n", pt_phys_val, pd_idx);
                }
 
                // Unmap temp PT mapping if not already done (because it wasn't freed)
                if (pt_virt && !pt_was_freed) {
                    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
                }
           } else {
                terminal_printf("[Unmap Error] Failed map PT (Phys=0x%x) for PDE %d\n", pt_phys_val, pd_idx);
                final_result = -1; // Mark error but continue
                v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip rest of this PT range
           }
     } // End PDE loop
 
     // Unmap target PD
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
 
     return final_result;
 }
 
 
  // Identity maps a range
  int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, uint32_t size, uint32_t flags) {
      // Ensure PAGE_PRESENT is set for identity maps
      return paging_map_range(page_directory_phys, start_addr, start_addr, size, flags | PAGE_PRESENT);
  }
 
 
  /* Page Fault Handler (#PF) */
  void page_fault_handler(registers_t *regs) {
      // (Keep existing page_fault_handler logic)
      // It should call find_vma and handle_vma_fault
      uint32_t fault_addr; asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
      uint32_t error_code = regs->err_code;
      bool present = (error_code & 0x1) != 0; bool write = (error_code & 0x2) != 0;
      bool user = (error_code & 0x4) != 0; bool reserved_bit = (error_code & 0x8) != 0;
      bool instruction_fetch = (error_code & 0x10) != 0;
      pcb_t* current_process = get_current_process();
      uint32_t current_pid = current_process ? current_process->pid : 0;
 
      terminal_printf("\n--- PAGE FAULT (PID %d) ---\n", current_pid);
      terminal_printf(" Addr: 0x%x Code: 0x%x (%s %s %s %s %s)\n", fault_addr, error_code,
                     present ? "P" : "NP", write ? "W" : "R", user ? "U" : "S",
                     reserved_bit ? "RSV" : "-", instruction_fetch ? "IF" : "-");
      terminal_printf(" EIP: 0x%x\n", regs->eip);
 
      if (reserved_bit) { terminal_write("  Error: Reserved bit violation!\n"); goto unhandled_fault; }
      if (!user && fault_addr < KERNEL_SPACE_VIRT_START) { terminal_write("  Error: Kernel fault in user-space!\n"); goto unhandled_fault; }
      if (!current_process || !current_process->mm) { terminal_write("  Error: Page fault outside valid process context!\n"); goto unhandled_fault; }
 
      mm_struct_t *mm = current_process->mm;
      vma_struct_t *vma = find_vma(mm, fault_addr);
 
      if (!vma) { terminal_printf("  Error: No VMA found for addr 0x%x. Segmentation Fault.\n", fault_addr); goto unhandled_fault; }
      terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);
 
      int result = handle_vma_fault(mm, vma, fault_addr, error_code); // Delegate to mm.c
 
      if (result == 0) { return; } // Success, retry instruction
      else { terminal_printf("  Error: handle_vma_fault failed (code %d). Segmentation Fault.\n", result); goto unhandled_fault; }
 
  unhandled_fault:
      terminal_write("--- Unhandled Page Fault ---\n");
      // Dump more registers if needed: regs->eax, ebx, etc.
      terminal_printf(" Terminating process (PID %d) due to page fault at 0x%x.\n", current_pid, fault_addr);
      terminal_printf("--------------------------\n");
      uint32_t exit_code = 0xFE000000 | error_code; // Unique exit code for page fault
      remove_current_task_with_code(exit_code);
      // Should not be reached
      terminal_write("[PANIC] Error: remove_current_task_with_code returned after page fault!\n");
      while(1) { __asm__ volatile("cli; hlt"); }
  }
 
 
  /**
   * @brief Frees all user-space page tables associated with a page directory.
   * Uses BUDDY_FREE to free the Page Table frames. Assumes data pages are freed elsewhere.
   */
  void paging_free_user_space(uint32_t *page_directory_phys) {
     if (!page_directory_phys || !g_kernel_page_directory_phys) {
         terminal_write("[Paging] free_user_space: Invalid PD or kernel PD not set.\n");
         return;
     }
 
     uint32_t *target_pd_virt = NULL;
 
     // Map target PD temporarily
     if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA) != 0) {
         terminal_write("[Paging] free_user_space: Could not map target PD!\n");
         return;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
 
     // Iterate through USER-SPACE page directory entries (0 up to KERNEL_PDE_INDEX)
     for (int i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t pde = target_pd_virt[i];
         if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) {
             // Free the 4KB page table frame using BUDDY
             uintptr_t pt_phys = pde & ~0xFFF;
             // terminal_printf("  [Cleanup] Freeing Page Table Frame: Phys=0x%x (from PDE %d)\n", pt_phys, i);
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
         } else if (pde & PAGE_PRESENT && pde & PAGE_SIZE_4MB) {
              terminal_printf("[Paging] free_user_space: Warning - Found 4MB page in user space PDE %d during cleanup. Data pages not freed here.\n", i);
         }
         // Clear the PDE entry (optional but good practice)
         target_pd_virt[i] = 0;
     }
 
     // Unmap target PD
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
  }
 
 
  // --- Paging Initialization ---
  // Initializes paging, allocates initial PD and PTs using buddy allocator.
  // Assumes physical memory access is possible for initial setup before paging_activate.
  int paging_init(uintptr_t kernel_phys_start, uintptr_t kernel_phys_end, uintptr_t total_memory_bytes) {
      terminal_write("[Paging] Initializing Paging...\n");
 
      // 1. Check/Enable PSE
      if (!check_and_enable_pse()) {
          // Continue without PSE if not supported, but log it.
          terminal_write("  [Paging Warning] PSE not supported or failed to enable. Using 4KB pages only.\n");
      }
 
      // 2. Allocate initial PD using BUDDY
      terminal_write("  [Paging] Allocating initial Page Directory frame...\n");
      void* pd_frame_ptr = BUDDY_ALLOC(PAGE_SIZE);
      if (!pd_frame_ptr) {
          terminal_write("  [FATAL] Failed to allocate frame for initial Page Directory!\n");
          return -1;
      }
      uintptr_t initial_pd_phys_addr = (uintptr_t)pd_frame_ptr;
      g_kernel_page_directory_phys = initial_pd_phys_addr;
      terminal_printf("  Allocated initial PD frame: Phys=0x%x\n", g_kernel_page_directory_phys);
 
      // --- Physical access assumption for initial setup ---
      terminal_write("  [Paging] Setting up initial PD self-mapping (physical access assumed)...\n");
      uint32_t* initial_pd_phys_ptr = (uint32_t*)g_kernel_page_directory_phys;
      memset(initial_pd_phys_ptr, 0, PAGE_SIZE); // Clear PD physically
 
      // Allocate PT for self-mapping using BUDDY
      void* pt_frame_ptr = BUDDY_ALLOC(PAGE_SIZE);
      if (!pt_frame_ptr) {
          terminal_write("  [FATAL] Failed to allocate PT for PD self-map!\n");
          BUDDY_FREE(pd_frame_ptr, PAGE_SIZE); // Free PD frame
          g_kernel_page_directory_phys = 0;
          return -1;
      }
      uintptr_t pd_map_pt_phys = (uintptr_t)pt_frame_ptr;
      uint32_t* pd_map_pt_phys_ptr = (uint32_t*)pd_map_pt_phys;
      memset(pd_map_pt_phys_ptr, 0, PAGE_SIZE); // Clear PT physically
 
      // Calculate indices for virtual PD address
      g_kernel_page_directory_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + g_kernel_page_directory_phys);
      uint32_t pd_virt_pde_idx = PDE_INDEX(g_kernel_page_directory_virt);
      uint32_t pd_virt_pte_idx = PTE_INDEX(g_kernel_page_directory_virt);
 
      // Set PTE in self-map PT to point to PD frame physically
      pd_map_pt_phys_ptr[pd_virt_pte_idx] = (g_kernel_page_directory_phys & ~0xFFF) | PTE_KERNEL_DATA;
      // Set PDE in PD to point to self-map PT physically
      initial_pd_phys_ptr[pd_virt_pde_idx] = (pd_map_pt_phys & ~0xFFF) | PTE_KERNEL_DATA;
      terminal_printf("  Self-map PT Phys=0x%x; PD mapped to Virt=0x%x via PDE %d\n",
                      pd_map_pt_phys, (uintptr_t)g_kernel_page_directory_virt, pd_virt_pde_idx);
      // --- End Physical access ---
 
      // --- Map Kernel Sections ---
      terminal_write("  [Paging] Mapping kernel sections...\n");
      uintptr_t kernel_virt_start = KERNEL_SPACE_VIRT_START + PAGE_ALIGN_DOWN(kernel_phys_start);
      uintptr_t kernel_map_start_phys = PAGE_ALIGN_DOWN(kernel_phys_start);
      uintptr_t kernel_map_end_phys = PAGE_ALIGN_UP(kernel_phys_end);
 
      for (uintptr_t p = kernel_map_start_phys, v = kernel_virt_start; p < kernel_map_end_phys; ) {
          bool use_large = g_pse_supported && (p % PAGE_SIZE_LARGE == 0) && (v % PAGE_SIZE_LARGE == 0) && ((kernel_map_end_phys - p) >= PAGE_SIZE_LARGE);
          size_t step = use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
          uint32_t pde_idx = PDE_INDEX(v);
 
          // Use physical pointer to PD for setup before paging_activate
          if (use_large) {
              initial_pd_phys_ptr[pde_idx] = (p & 0xFFC00000) | PAGE_PRESENT | PAGE_RW | PAGE_SIZE_4MB;
          } else {
              if (!(initial_pd_phys_ptr[pde_idx] & PAGE_PRESENT)) {
                   void* pt_kernel_ptr = BUDDY_ALLOC(PAGE_SIZE);
                   if (!pt_kernel_ptr) { /* Fatal: Cleanup needed */ return -1; }
                   uintptr_t pt_phys = (uintptr_t)pt_kernel_ptr;
                   memset((void*)pt_phys, 0, PAGE_SIZE);
                   initial_pd_phys_ptr[pde_idx] = (pt_phys & ~0xFFF) | PTE_KERNEL_DATA;
              }
              uintptr_t pt_phys_addr_for_pte = initial_pd_phys_ptr[pde_idx] & ~0xFFF;
              uint32_t* pt_phys_ptr_for_pte = (uint32_t*)pt_phys_addr_for_pte;
              uint32_t pte_idx = PTE_INDEX(v);
              pt_phys_ptr_for_pte[pte_idx] = (p & ~0xFFF) | PTE_KERNEL_DATA;
          }
          p += step;
          v += step;
      }
 
      // --- Map initial physical memory range (Identity Map Only Initially) ---
      terminal_write("  [Paging] Identity mapping initial physical memory...\n");
      uint32_t initial_map_size = (total_memory_bytes > (16*1024*1024)) ? (16*1024*1024) : total_memory_bytes;
      initial_map_size = ALIGN_UP(initial_map_size, PAGE_SIZE);
 
      for (uintptr_t addr = 0; addr < initial_map_size; ) {
          bool use_large = g_pse_supported && (addr % PAGE_SIZE_LARGE == 0) && ((initial_map_size - addr) >= PAGE_SIZE_LARGE);
          size_t step = use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
          uint32_t pde_idx = PDE_INDEX(addr);
 
           if (use_large) {
               initial_pd_phys_ptr[pde_idx] = (addr & 0xFFC00000) | PTE_KERNEL_DATA | PAGE_SIZE_4MB;
           } else {
               if (!(initial_pd_phys_ptr[pde_idx] & PAGE_PRESENT)) {
                   void* pt_identity_ptr = BUDDY_ALLOC(PAGE_SIZE);
                   if (!pt_identity_ptr) { /* Fatal: Cleanup needed */ return -1; }
                   uintptr_t pt_phys = (uintptr_t)pt_identity_ptr;
                   memset((void*)pt_phys, 0, PAGE_SIZE);
                   initial_pd_phys_ptr[pde_idx] = (pt_phys & ~0xFFF) | PTE_KERNEL_DATA;
              }
              uintptr_t pt_phys_addr_for_pte = initial_pd_phys_ptr[pde_idx] & ~0xFFF;
              uint32_t* pt_phys_ptr_for_pte = (uint32_t*)pt_phys_addr_for_pte;
              uint32_t pte_idx = PTE_INDEX(addr);
              pt_phys_ptr_for_pte[pte_idx] = (addr & ~0xFFF) | PTE_KERNEL_DATA;
           }
          addr += step;
      }
 
      // 5. Activate paging
      terminal_write("  Activating Paging...\n");
      paging_activate((uint32_t*)g_kernel_page_directory_phys);
      terminal_write("  [OK] Paging Enabled.\n");
 
      // --- Now Paging is ON, map the higher-half physical range ---
      terminal_write("  [Paging] Mapping initial physical memory (higher half)...\n");
      if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, KERNEL_SPACE_VIRT_START, 0, initial_map_size, PTE_KERNEL_DATA) != 0) {
           terminal_write("  [FATAL] Failed to map initial physical memory (higher half)!\n");
           return -1; // Or attempt recovery?
      }
 
      terminal_write("[Paging] Initialization complete.\n");
      return 0; // Success
  }