/**
 * paging.c - Paging Implementation with PSE (4MB Pages) support.
 */

 #include "paging.h"
 #include "frame.h"
 #include "terminal.h"
 #include "types.h"
 #include "process.h"
 #include "mm.h"
 #include "scheduler.h"
 #include <string.h>
 #include "cpuid.h" // Assuming a simple cpuid.h helper exists

 // --- Globals ---
 // Pointers to the kernel's page directory (set by paging_init)
uint32_t* g_kernel_page_directory_virt = NULL;
uint32_t g_kernel_page_directory_phys = 0;
 // Flag indicating if PSE (4MB pages) is supported and enabled
 bool g_pse_supported = false;

 // Temporary virtual addresses for mapping PD/PT during operations
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)


 // --- Helper Macros ---
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 #define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
 #define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))


 // --- Forward Declarations ---
 static uint32_t* allocate_page_table_phys(void);
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);

 // --- CPU Feature Detection and Control ---

 // Read CR4 register
 static inline uint32_t read_cr4(void) {
    uint32_t val;
    asm volatile("mov %%cr4, %0" : "=r"(val));
    return val;
 }

 // Write CR4 register
 static inline void write_cr4(uint32_t value) {
    asm volatile("mov %0, %%cr4" : : "r"(value));
 }

 // Enable PSE bit (bit 4) in CR4
 static inline void enable_cr4_pse(void) {
    write_cr4(read_cr4() | (1 << 4));
 }

 // Checks CPUID for PSE support and enables it in CR4
 bool check_and_enable_pse(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx); // Assuming cpuid helper exists

    if (edx & (1 << 3)) { // Check EDX bit 3 for PSE support
        terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
        enable_cr4_pse(); // Enable PSE in CR4
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
     // Note: SMP requires TLB shootdown
     asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }

 void tlb_flush_range(void* start, size_t size) {
     // Note: SMP requires TLB shootdown
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


 /**
  * @brief Internal helper to map a single page (4KB or 4MB).
  * Manages temporary mappings of PD and PT.
  * Handles PSE flag for large pages.
  */
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page) {
     if (!page_directory_phys) return -1;
     uint32_t pd_idx = PDE_INDEX(vaddr);

     // Align addresses based on page size requested
     if (use_large_page) {
         vaddr = PAGE_LARGE_ALIGN_DOWN(vaddr);
         paddr = PAGE_LARGE_ALIGN_DOWN(paddr);
         if (!g_pse_supported) {
             terminal_printf("map_page_internal: Error - Attempted large page map but PSE not supported/enabled.\n");
             return -1;
         }
         flags |= PAGE_SIZE_4MB; // Set the PS bit in flags
     } else {
         vaddr = PAGE_ALIGN_DOWN(vaddr);
         paddr = PAGE_ALIGN_DOWN(paddr);
         flags &= ~PAGE_SIZE_4MB; // Ensure PS bit is clear
     }

     uint32_t* target_pd_virt = NULL;
     uint32_t* page_table_virt = NULL;
     uint32_t* pt_phys = NULL;
     int ret = -1;

     // Map target PD temporarily
     if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA, false) != 0) {
         terminal_write("map_page_internal: Failed temp map PD\n");
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = target_pd_virt[pd_idx];

     // --- Handle Large Page Mapping ---
     if (use_large_page) {
         if (pde & PAGE_PRESENT) {
             // PDE already exists. Error if it's mapping a PT (cannot mix sizes).
             if (!(pde & PAGE_SIZE_4MB)) {
                 terminal_printf("map_page_internal: Error - Cannot map 4MB page, PDE %d already points to a Page Table.\n", pd_idx);
                 goto cleanup_map_page_pd;
             }
             // Overwriting existing 4MB page? Check if physical address matches? Error for now.
             terminal_printf("map_page_internal: Error - PDE %d already maps a 4MB page.\n", pd_idx);
             goto cleanup_map_page_pd;
         }
         // Set PDE directly for 4MB page
         target_pd_virt[pd_idx] = (paddr & 0xFFC00000) | (flags & 0xFFF) | PAGE_SIZE_4MB | PAGE_PRESENT;
         ret = 0; // Success
         // Invalidate TLB for the entire 4MB range
         tlb_flush_range((void*)vaddr, PAGE_SIZE_LARGE);
         goto cleanup_map_page_pd; // Skip PT handling
     }

     // --- Handle 4KB Page Mapping ---
     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)) {
         // Error: Trying to map 4KB page over an existing 4MB page.
         terminal_printf("map_page_internal: Error - Cannot map 4KB page, PDE %d already maps a 4MB page.\n", pd_idx);
         goto cleanup_map_page_pd;
         // TODO: Implement breaking the 4MB page if needed (complex).
     }

     // Ensure Page Table Exists
     if (!(pde & PAGE_PRESENT)) {
         pt_phys = allocate_page_table_phys();
         if (!pt_phys) { goto cleanup_map_page_pd; }
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER; // Default flags for new PT
         if (!(flags & PAGE_USER)) { pde_flags &= ~PAGE_USER; }
         target_pd_virt[pd_idx] = (uint32_t)pt_phys | pde_flags;
         pde = target_pd_virt[pd_idx];
         paging_invalidate_page((void*)vaddr); // Invalidate TLB for changed PDE range
     } else {
         pt_phys = (uint32_t*)(pde & ~0xFFF);
         uint32_t needed_pde_flags = (flags & (PAGE_USER | PAGE_RW));
         if ((pde & needed_pde_flags) != needed_pde_flags) {
             target_pd_virt[pd_idx] |= needed_pde_flags;
             paging_invalidate_page((void*)vaddr); // Invalidate TLB for changed PDE flags
         }
     }

     // Map the Page Table temporarily
     if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA, false) != 0) {
         terminal_write("map_page_internal: Failed temp map PT\n");
         goto cleanup_map_page_pd;
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
     uint32_t pt_idx = PTE_INDEX(vaddr);

     // Check if PTE already exists
     if (page_table_virt[pt_idx] & PAGE_PRESENT) {
          terminal_printf("map_page_internal: Error - PTE already present for vaddr 0x%x!\n", vaddr);
          goto cleanup_map_page_pt; // Error
     }

     // Set the Page Table Entry
     page_table_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     ret = 0; // Success

 cleanup_map_page_pt:
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE); // Unmap temp PT
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);

 cleanup_map_page_pd:
     paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE); // Unmap temp PD
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

     if (ret == 0) {
         paging_invalidate_page((void*)vaddr); // Invalidate specific page on success
     }

     return ret;
 }

 // Public function: Maps a single 4KB page
 int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
     return map_page_internal(page_directory_phys, vaddr, paddr, flags, false); // Use 4KB page
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

         if (can_use_large) {
             // terminal_printf("paging_map_range: Using 4MB page for V=0x%x -> P=0x%x\n", v, p);
             if (map_page_internal(page_directory_phys, v, p, flags, true) != 0) {
                 terminal_printf("paging_map_range: Failed mapping 4MB page V=0x%x -> P=0x%x\n", v, p);
                 return -1; // TODO: Rollback?
             }
             v += PAGE_SIZE_LARGE;
             p += PAGE_SIZE_LARGE;
         } else {
             // terminal_printf("paging_map_range: Using 4KB page for V=0x%x -> P=0x%x\n", v, p);
             if (map_page_internal(page_directory_phys, v, p, flags, false) != 0) {
                 terminal_printf("paging_map_range: Failed mapping 4KB page V=0x%x -> P=0x%x\n", v, p);
                 return -1; // TODO: Rollback?
             }
             v += PAGE_SIZE;
             p += PAGE_SIZE;
         }
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
  */
  int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz) {
    if (!page_directory_phys || memsz == 0) return -1;
    uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
    uintptr_t virt_end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
    if (virt_end <= virt_start) return -1;

    // terminal_printf("paging_unmap_range: PD=0x%x, V=[0x%x-0x%x)\n", page_directory_phys, virt_start, virt_end);

    uint32_t *target_pd_virt = NULL;
    uint32_t *pt_virt = NULL;
    int result = 0;

    // Map target PD temporarily
    if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA, false) != 0) {
        return -1;
    }
    target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;

    for (uintptr_t v = virt_start; v < virt_end; ) {
          uint32_t pd_idx = PDE_INDEX(v);
          uint32_t pde = target_pd_virt[pd_idx];

          if (!(pde & PAGE_PRESENT)) {
              v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip to next PDE if not present
              continue;
          }

          // --- Handle 4MB Page Unmapping ---
          if (pde & PAGE_SIZE_4MB) {
               uintptr_t large_page_v_start = PAGE_LARGE_ALIGN_DOWN(v);
               if (large_page_v_start >= virt_start && (large_page_v_start + PAGE_SIZE_LARGE) <= virt_end) {
                    // Unmap the full 4MB page PDE
                    terminal_printf("  Unmapping 4MB Page: V=0x%x (PDE %d)\n", large_page_v_start, pd_idx);
                    uintptr_t frame_base_phys = pde & 0xFFC00000;

                    // Call put_frame for each underlying 4KB frame
                    for (int i = 0; i < 1024; ++i) {
                        put_frame(frame_base_phys + i * PAGE_SIZE);
                    }

                    target_pd_virt[pd_idx] = 0; // Clear PDE
                    tlb_flush_range((void*)large_page_v_start, PAGE_SIZE_LARGE); // Flush entire 4MB range
                    v = large_page_v_start + PAGE_SIZE_LARGE; // Advance by 4MB
               } else {
                    // Partial unmap of a 4MB page requires breaking it - not implemented.
                    terminal_printf("  Error: Cannot partially unmap a 4MB page (V=0x%x, PDE %d).\n", v, pd_idx);
                    result = -1; // Error: Cannot handle partial 4MB unmap
                    v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip to next PDE
               }
               continue; // Move to next iteration
          }

          // --- Handle 4KB Page Unmapping (within a Page Table) ---
          uint32_t* pt_phys = (uint32_t*)(pde & ~0xFFF);
          pt_virt = NULL;
          bool pt_was_freed = false;

          // Map PT temporarily
          if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA, false) == 0) {
               pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;

               // Iterate through PTEs within this PT relevant to the current unmap range
               uintptr_t pt_range_end = (PDE_INDEX(v) == PDE_INDEX(virt_end - 1)) ? virt_end : (PAGE_ALIGN_DOWN(v) + PAGE_SIZE_LARGE);
               while (v < pt_range_end && v < virt_end) {
                    uint32_t pt_idx = PTE_INDEX(v);
                    uint32_t pte = pt_virt[pt_idx];
                    if (pte & PAGE_PRESENT) {
                         uintptr_t frame_phys = pte & ~0xFFF;
                         put_frame(frame_phys);
                         pt_virt[pt_idx] = 0;
                         paging_invalidate_page((void*)v);
                    }
                    v += PAGE_SIZE; // Move to the next page
               } // End PTE loop

               // Check if PT became empty
               if (is_page_table_empty(pt_virt)) {
                    target_pd_virt[pd_idx] = 0; // Clear PDE
                    tlb_flush_range((void*)PAGE_ALIGN_DOWN(v - PAGE_SIZE), PAGE_SIZE_LARGE); // Flush PDE range

                    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE); // Unmap temp PT
                    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
                    pt_virt = NULL;
                    pt_was_freed = true;

                    put_frame((uintptr_t)pt_phys); // Free PT frame
                    terminal_printf("  Freed empty page table (Phys=0x%x) for PDE %d\n", pt_phys, pd_idx);
               }

               // Unmap temp PT mapping if not freed
               if (pt_virt && !pt_was_freed) {
                   paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                   paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
               }
          } else {
               terminal_printf("paging_unmap_range: Error mapping PT (Phys=0x%x) for PDE %d\n", pt_phys, pd_idx);
               result = -1;
               v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE); // Skip rest of this PT range
          }
    } // End for loop

    // Unmap target PD
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

    return result;
}


 // Identity maps a range
 int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, uint32_t size, uint32_t flags) {
     // Use default 4KB pages for identity mapping unless explicitly requested otherwise
     return paging_map_range(page_directory_phys, start_addr, start_addr, size, flags | PAGE_PRESENT);
 }


 /* Page Fault Handler (#PF) */
 void page_fault_handler(registers_t *regs) {
     // --- Fault Info ---
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

     // --- Basic Checks ---
     if (reserved_bit) {
          terminal_write("  Error: Reserved bit violation in page entry!\n");
          goto unhandled_fault;
     }
     if (!user && fault_addr < KERNEL_SPACE_VIRT_START) {
          terminal_write("  Error: Kernel fault in user-space address range!\n");
          goto unhandled_fault; // Likely kernel bug
     }

     // --- Get Process Context ---
     if (!current_process || !current_process->mm) {
         terminal_write("  Error: Page fault outside valid process context!\n");
         goto unhandled_fault; // Cannot handle without mm_struct
     }
     mm_struct_t *mm = current_process->mm;

     // --- Find VMA ---
     vma_struct_t *vma = find_vma(mm, fault_addr); // Handles locking

     if (!vma) {
         terminal_printf("  Error: No VMA found for addr 0x%x. Segmentation Fault.\n", fault_addr);
         goto unhandled_fault;
     }
     terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);

     // --- Delegate to VMA Fault Handler ---
     int result = handle_vma_fault(mm, vma, fault_addr, error_code); // In mm.c

     if (result == 0) { return; } // Success, retry instruction
     else {
         terminal_printf("  Error: handle_vma_fault failed (code %d). Segmentation Fault.\n", result);
         goto unhandled_fault;
     }

 unhandled_fault:
     terminal_write("--- Unhandled Page Fault ---\n");
     terminal_printf(" Terminating process (PID %d) due to page fault at 0x%x.\n", current_pid, fault_addr);
     terminal_printf("--------------------------\n");
     uint32_t exit_code = 0xFE000000 | error_code;
     remove_current_task_with_code(exit_code);
     // Should not be reached
     terminal_write("[PANIC] Error: remove_current_task_with_code returned after page fault!\n");
     while(1) { __asm__ volatile("cli; hlt"); }
 }


 /**
  * @brief Frees all user-space page tables associated with a page directory.
  */
 void paging_free_user_space(uint32_t *page_directory_phys) {
    if (!page_directory_phys || !g_kernel_page_directory_phys) return;
    // terminal_printf("[Paging] Freeing user space for PD Phys=0x%x\n", page_directory_phys);

    uint32_t *target_pd_virt = NULL;

    // Map target PD temporarily
    if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA, false) != 0) {
        terminal_write("  Error: Could not map target PD for cleanup!\n");
        return;
    }
    target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;

    // Iterate through USER-SPACE page directory entries
    for (int i = 0; i < KERNEL_PDE_INDEX; ++i) {
        uint32_t pde = target_pd_virt[i];
        if (pde & PAGE_PRESENT) {
            if (pde & PAGE_SIZE_4MB) {
                // Handle 4MB page unmapping
                uintptr_t frame_base_phys = pde & 0xFFC00000;
                 terminal_printf("  Freeing 4MB Page Frame Range: Phys=0x%x (from PDE %d)\n", frame_base_phys, i);
                 // Call put_frame for each underlying 4KB frame
                 for (int j = 0; j < 1024; ++j) {
                     put_frame(frame_base_phys + j * PAGE_SIZE);
                 }
            } else {
                // Handle 4KB page table
                uintptr_t pt_phys = pde & ~0xFFF;
                 terminal_printf("  Freeing Page Table Frame: Phys=0x%x (from PDE %d)\n", pt_phys, i);
                // Free the page table frame itself. Assumes data pages within were handled.
                put_frame(pt_phys);
            }
            // Clear the PDE entry (optional but good practice)
            target_pd_virt[i] = 0;
        }
    }

    // Unmap target PD
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PD, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);

    // terminal_write("[Paging] User space cleanup finished.\n");
 }

 // --- Static Helper Implementations ---

 // Allocates PT frame, returns physical address
 static uint32_t* allocate_page_table_phys(void) {
    uintptr_t pt_phys = frame_alloc();
    if (!pt_phys) { return NULL; }
    // Map temporarily to zero it
    if (map_page_internal((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA, false) != 0) {
         put_frame(pt_phys); return NULL;
    }
    memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
    paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
    return (uint32_t*)pt_phys;
}


 // --- Paging Initialization ---
 int paging_init(uintptr_t kernel_phys_start, uintptr_t kernel_phys_end, uintptr_t total_memory_bytes) {
     terminal_write("[Paging] Initializing Paging...\n");

     // 1. Check for and enable PSE support
     check_and_enable_pse(); // Sets g_pse_supported

     // 2. Allocate initial Page Directory (needs frame_alloc)
     uintptr_t initial_pd_phys_addr = frame_alloc();
     if (!initial_pd_phys_addr) {
         terminal_write("  [FATAL] Failed to allocate frame for initial Page Directory!\n");
         return -1;
     }
     g_kernel_page_directory_phys = initial_pd_phys_addr;
     terminal_printf("  Allocated initial PD frame: Phys=0x%x\n", g_kernel_page_directory_phys);

     // We cannot easily map it virtually *before* paging is on.
     // So, we have to work with the physical address initially, or use a temporary
     // identity map setup by the bootloader (if available). Assuming we must construct
     // it using physical addresses. This is complex.

     // *** SIMPLIFICATION for this context: Assume frame_alloc returns a pointer
     //     that is currently identity mapped OR we map it temporarily. Let's map it.
     //     This requires a primitive mapping function usable before full paging.
     //     Let's assume `early_temp_map(phys)` exists or proceed carefully.
     //     For now, let's map it *after* enabling paging using the PD itself.

     // Temporarily map the new PD so we can write to it
     // We map it at its eventual virtual address in the higher half
     g_kernel_page_directory_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + g_kernel_page_directory_phys);

     // Hacky: Map the PD page into itself temporarily *before* enabling paging fully.
     // This relies on specific address calculations and is fragile.
     // A cleaner way involves bootloader pre-mapping or more complex initial setup.
     // Map PD physical address -> KERNEL_SPACE_VIRT_START + physical address
     // PDE Index for PD's Virtual Address:
     uint32_t pd_virt_pde_idx = PDE_INDEX(g_kernel_page_directory_virt);
     // PTE Index for PD's Virtual Address:
     uint32_t pd_virt_pte_idx = PTE_INDEX(g_kernel_page_directory_virt);

     // We need a Page Table to map the PD's virtual address. Allocate PT frame.
     uintptr_t pd_map_pt_phys = frame_alloc();
     if (!pd_map_pt_phys) { /* Fatal Error */ return -1; }
     uint32_t* pd_map_pt_ptr = (uint32_t*)pd_map_pt_phys; // Treat phys addr as ptr temporarily

     // Clear the new PT (Need temporary mapping or physical access method)
     // Assume physical access or early map allows clearing pd_map_pt_ptr[0..1023] = 0;
      memset((void*)pd_map_pt_phys, 0, PAGE_SIZE); // DANGEROUS if not identity mapped!

     // Set the PTE within this PT to map the PD frame
     pd_map_pt_ptr[pd_virt_pte_idx] = (g_kernel_page_directory_phys & ~0xFFF) | PTE_KERNEL_DATA;

     // Now, point the corresponding PDE in the *physical* PD to this new PT
     uint32_t* initial_pd_phys_ptr = (uint32_t*)g_kernel_page_directory_phys;
     initial_pd_phys_ptr[pd_virt_pde_idx] = (pd_map_pt_phys & ~0xFFF) | PTE_KERNEL_DATA;

     // --- Now g_kernel_page_directory_virt should be accessible ---
     // Clear the rest of the virtual PD mapping
     memset(g_kernel_page_directory_virt, 0, PAGE_SIZE);
     // Restore the self-mapping PDE entry
     g_kernel_page_directory_virt[pd_virt_pde_idx] = (pd_map_pt_phys & ~0xFFF) | PTE_KERNEL_DATA;


     terminal_printf("  Kernel PD Virt Addr: 0x%x\n", g_kernel_page_directory_virt);

     // 3. Map Kernel Sections using 4MB pages if possible
     terminal_write("  Mapping Kernel...\n");
     uintptr_t kernel_virt_start = KERNEL_SPACE_VIRT_START; // Kernel starts here virtually
     uintptr_t kernel_map_start_phys = PAGE_LARGE_ALIGN_DOWN(kernel_phys_start);
     uintptr_t kernel_map_end_phys = PAGE_LARGE_ALIGN_UP(kernel_phys_end);
     uintptr_t current_virt = kernel_virt_start + kernel_map_start_phys; // Adjust virt start based on alignment

     for (uintptr_t p = kernel_map_start_phys; p < kernel_map_end_phys; ) {
         uint32_t pde_idx = PDE_INDEX(current_virt);
         if (g_pse_supported && (p % PAGE_SIZE_LARGE == 0) && (current_virt % PAGE_SIZE_LARGE == 0)) {
             // Map using 4MB page
             g_kernel_page_directory_virt[pde_idx] = (p & 0xFFC00000) | PAGE_PRESENT | PAGE_RW | PAGE_SIZE_4MB; // Kernel R/W
             // terminal_printf("    Mapped Kernel 4MB: V=0x%x -> P=0x%x (PDE %d)\n", current_virt, p, pde_idx);
             p += PAGE_SIZE_LARGE;
             current_virt += PAGE_SIZE_LARGE;
         } else {
             // Map using 4KB pages (requires PT)
             // Allocate PT if needed
             if (!(g_kernel_page_directory_virt[pde_idx] & PAGE_PRESENT)) {
                  uint32_t* pt_phys = allocate_page_table_phys();
                  if (!pt_phys) { /* Fatal Error */ return -1; }
                  g_kernel_page_directory_virt[pde_idx] = ((uintptr_t)pt_phys & ~0xFFF) | PTE_KERNEL_DATA; // PDE points to PT
             }
             // Map PT temporarily to set PTE
             uint32_t* pt_phys_addr = (uint32_t*)(g_kernel_page_directory_virt[pde_idx] & ~0xFFF);
              if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys_addr, PTE_KERNEL_DATA) != 0) {
                   /* Fatal Error */ return -1;
              }
              uint32_t* pt_virt_map = (uint32_t*)TEMP_MAP_ADDR_PT;
              uint32_t pte_idx = PTE_INDEX(current_virt);
              pt_virt_map[pte_idx] = (p & ~0xFFF) | PTE_KERNEL_DATA; // Map 4KB page
              paging_unmap_range((uint32_t*)g_kernel_page_directory_phys, TEMP_MAP_ADDR_PT, PAGE_SIZE);
              paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
              // terminal_printf("    Mapped Kernel 4KB: V=0x%x -> P=0x%x (PDE %d, PTE %d)\n", current_virt, p, pde_idx, pte_idx);

             p += PAGE_SIZE;
             current_virt += PAGE_SIZE;
         }
     }

     // 4. Map initial physical memory range (e.g., first 16MB or up to total_memory)
     //    Map both identity and higher-half for easy access to memory managed by buddy/frame.
     uint32_t initial_map_size = (total_memory_bytes > (16*1024*1024)) ? (16*1024*1024) : total_memory_bytes;
     initial_map_size = PAGE_LARGE_ALIGN_UP(initial_map_size); // Align size up
     terminal_printf("  Mapping Initial %u MB Physical Memory...\n", initial_map_size / (1024*1024));
     // Identity map [0..initial_map_size) -> [0..initial_map_size)
     paging_map_range((uint32_t*)g_kernel_page_directory_phys, 0, 0, initial_map_size, PTE_KERNEL_DATA);
     // Higher-half map [0..initial_map_size) -> [KERNEL_START..KERNEL_START+initial_map_size)
     paging_map_range((uint32_t*)g_kernel_page_directory_phys, KERNEL_SPACE_VIRT_START, 0, initial_map_size, PTE_KERNEL_DATA);

     // 5. Activate paging
     terminal_write("  Activating Paging...\n");
     paging_activate((uint32_t*)g_kernel_page_directory_phys);
     terminal_write("  [OK] Paging Enabled.\n");

     return 0; // Success
 }