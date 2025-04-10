/**
 * paging.c - Paging Implementation with VMM improvements.
 */

 #include "paging.h"
 #include "buddy.h"
 #include "terminal.h"
 #include "types.h"
 #include "process.h" // For pcb_t definition, get_current_process()
 #include "mm.h"      // For mm_struct_t, vma_struct_t, find_vma, handle_vma_fault
 #include <string.h>    // For memset
 #include "frame.h"
 
 // Global Kernel Page Directory (Virtual Address)
 uint32_t* kernel_page_directory = NULL;
 
 // Temporary virtual address for mapping page tables/directories
 // Ensure these addresses are reserved and not used elsewhere in kernel space.
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
 
 // --- Helper Macros & Basic Functions (as before) ---
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 
 // Declarations for static helpers
 static uint32_t* allocate_page_table_phys(void);
 static bool is_page_table_empty(uint32_t* pt_virt);
 
 // --- Public API Functions ---
 
 void paging_set_directory(uint32_t* pd_virt) { kernel_page_directory = pd_virt; }
 void paging_invalidate_page(void *vaddr) { asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory"); }
 void tlb_flush_range(void* start, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr = ALIGN_UP((uintptr_t)start + size, PAGE_SIZE);
     while (addr < end_addr) { paging_invalidate_page((void*)addr); addr += PAGE_SIZE; }
 }
 void paging_activate(uint32_t *page_directory_phys) { /* ... as before ... */
     uint32_t cr0;
     asm volatile("mov %0, %%cr3" : : "r"(page_directory_phys) : "memory");
     asm volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit
     asm volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 
 // Maps a single page (physical -> virtual) in a given page directory (virtual address)
 int paging_map_single(uint32_t *page_directory_virt, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
     if (!page_directory_virt) return -1;
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t pde = page_directory_virt[pd_idx];
     uint32_t* page_table_virt = NULL;
     uint32_t* pt_phys = NULL; // Store physical address
 
     bool pde_modified = false; // Track if PDE needs TLB flush later (if its flags change)
 
     if (!(pde & PAGE_PRESENT)) {
         // PT not present, allocate
         pt_phys = allocate_page_table_phys(); // Returns physical address
         if (!pt_phys) return -1;
 
         // Map PT temporarily to access its contents
         if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) != 0) {
              buddy_free(pt_phys, PAGE_SIZE); return -1;
         }
         page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
 
         // Update PDE in target directory
         uint32_t pde_flags = PAGE_PRESENT | (flags & (PAGE_USER | PAGE_RW)); // Inherit User/RW flags
         page_directory_virt[pd_idx] = (uint32_t)pt_phys | pde_flags;
         pde_modified = true;
 
     } else {
         // PT exists
         pt_phys = (uint32_t*)(pde & ~0xFFF); // Get physical address
 
         // Map PT temporarily to access its contents
         if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) != 0) {
              return -1;
         }
         page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
 
         // Ensure PDE flags are sufficient (e.g., add User/RW if needed)
         uint32_t new_pde_flags = pde | (flags & (PAGE_USER | PAGE_RW)) | PAGE_PRESENT;
         if (page_directory_virt[pd_idx] != new_pde_flags) {
              page_directory_virt[pd_idx] = new_pde_flags;
              pde_modified = true;
         }
     }
 
     // --- Set Page Table Entry ---
     page_table_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
 
     // Unmap the temporary PT mapping
     paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE); // Use unmap here (safe recursion depth=1)
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
 
     // Invalidate TLB for the specific virtual address mapped (caller or context switch handles broader flushes)
     // If the PDE flags changed, a TLB flush affecting the PDE might be needed,
     // but modifying a PTE covering 'vaddr' already requires invalidating 'vaddr'.
     // paging_invalidate_page((void*)PAGE_ALIGN_DOWN(vaddr)); // Already done by caller/PF handler?
 
     return 0;
 }
 
 // Maps a range using paging_map_single
 int paging_map_range(uint32_t *page_directory_virt, uint32_t virt_start_addr,
                      uint32_t phys_start_addr, uint32_t memsz, uint32_t flags)
 {
     // ... (Implementation as before, uses paging_map_single) ...
     if (!page_directory_virt || memsz == 0) { return -1; }
     uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t phys_start = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t virt_end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
     if (virt_end <= virt_start) { return -1; }
 
     for (uintptr_t v = virt_start, p = phys_start; v < virt_end; v += PAGE_SIZE, p += PAGE_SIZE) {
         if (paging_map_single(page_directory_virt, v, p, flags) != 0) {
             // TODO: Rollback previous mappings in this range
             return -1;
         }
     }
     return 0;
 }
 
 /**
  * @brief Checks if all PTEs within a given page table are non-present.
  *
  * @param pt_virt Virtual address of the page table to check.
  * @return true if empty, false otherwise.
  */
 static bool is_page_table_empty(uint32_t* pt_virt) {
     if (!pt_virt) return true; // Or false depending on desired behavior for NULL?
     for (int i = 0; i < 1024; ++i) {
         if (pt_virt[i] & PAGE_PRESENT) {
             return false; // Found a present entry
         }
     }
     return true; // All entries are non-present
 }
 
 /**
  * @brief Unmaps a range of virtual addresses. Frees physical frames and page tables if empty.
  *
  * @param page_directory_virt Virtual address of the page directory to modify.
  * @param virt_start_addr Start virtual address (page-aligned).
  * @param memsz Size of the range in bytes (page-aligned).
  * @return 0 on success, negative value on failure.
  */
  int paging_unmap_range(uint32_t *page_directory_virt, uint32_t virt_start_addr, uint32_t memsz) {
    // ... (loop structure as before) ...
    for (uintptr_t v = virt_start; v < virt_end; v += PAGE_SIZE) {
          uint32_t pd_idx = PDE_INDEX(v);
          uint32_t pde = page_directory_virt[pd_idx];
          if (!(pde & PAGE_PRESENT)) continue;
          uint32_t* pt_phys = (uint32_t*)(pde & ~0xFFF);

          // Map PT temporarily
          if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) == 0) {
               uint32_t* pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
               uint32_t pt_idx = PTE_INDEX(v);
               uint32_t pte = pt_virt[pt_idx];

               if (pte & PAGE_PRESENT) {
                    uint32_t frame_phys = pte & ~0xFFF;
                    // *** Use put_frame instead of buddy_free ***
                    put_frame(frame_phys);

                    pt_virt[pt_idx] = 0; // Clear PTE
                    paging_invalidate_page((void*)v); // Invalidate TLB

                    // Check if PT is empty
                    if (is_page_table_empty(pt_virt)) {
                         page_directory_virt[pd_idx] = 0; // Clear PDE
                         paging_invalidate_page((void*)v); // Invalidate TLB again

                         // Unmap temp PT *before* calling put_frame on PT
                         paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                         paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);

                         // *** Use put_frame for the Page Table page ***
                         put_frame((uintptr_t)pt_phys);

                         continue; // Skip final unmap/invalidate below
                    }
               }
               // Unmap temp PT mapping (if not freed above)
               paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE);
               paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
          } else { /* Log error mapping PT */ }
    }
    return 0;
}
 
 
 /* Creates identity map [0..size) */
 int paging_init_identity_map(uint32_t *page_directory_virt, uint32_t size, uint32_t flags) {
     // Use KERNEL flags for initial identity mapping
     return paging_map_range(page_directory_virt, 0, 0, size, flags | PAGE_PRESENT);
 }
 
 
 /* Page Fault Handler (#PF) - Uses VMA lookup */
 void page_fault_handler(registers_t *regs) {
     uint32_t fault_addr;
     asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
     uint32_t error_code = regs->err_code;
 
     bool present = error_code & 0x1;
     bool write = error_code & 0x2;
     bool user = error_code & 0x4;
 
     // --- Print Fault Info ---
     terminal_printf("\n--- PAGE FAULT (PID %d?) ---\n", get_current_process() ? get_current_process()->pid : 0);
     terminal_printf(" Addr: 0x%x Code: 0x%x (%s %s %s)\n", fault_addr, error_code,
                    present ? "P" : "NP", write ? "W" : "R", user ? "U" : "S");
     terminal_printf(" EIP: 0x%x\n", regs->eip);
 
     // --- Get Process Context ---
     pcb_t* current_process = get_current_process();
     if (!current_process || !current_process->mm) {
         terminal_write("  Error: Page fault outside valid process context!\n");
         goto unhandled_fault;
     }
     mm_struct_t *mm = current_process->mm;
 
     // --- Find VMA ---
     vma_struct_t *vma = find_vma(mm, fault_addr); // find_vma handles locking
 
     if (!vma) {
         terminal_printf("  Error: No VMA found for addr 0x%x. Segmentation Fault.\n", fault_addr);
         goto unhandled_fault;
     }
 
     terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);
 
     // --- Delegate to VMA Fault Handler ---
     int result = handle_vma_fault(mm, vma, fault_addr, error_code);
 
     if (result == 0) {
         // terminal_write("--- Page Fault Handled ---\n");
         return; // Success, return to retry instruction
     } else {
         terminal_printf("  Error: handle_vma_fault failed (code %d). Segmentation Fault.\n", result);
         goto unhandled_fault;
     }
 
 unhandled_fault:
     terminal_write("--- Unhandled Page Fault ---\n");
     terminal_printf("System Halted due to unhandled page fault at 0x%x.\n", fault_addr);
     // TODO: Implement process termination instead of halting
     asm volatile ("cli; hlt");
 }
 
 // --- Static Helper Implementations ---
 
 // Allocates PT, maps temp, clears, unmaps temp, returns phys addr
 static uint32_t* allocate_page_table_phys(void) {
    uintptr_t pt_phys = frame_alloc(); // Use frame_alloc()
    if (!pt_phys) {
        terminal_write("[Paging] ERROR: allocate_page_table: frame_alloc failed!\n");
        return 0;
    }
    // Map temporarily, clear, unmap (as before)
    if (paging_map_single(kernel_page_directory, TEMP_MAP_ADDR_PT, (uint32_t)pt_phys, PTE_KERNEL_DATA) != 0) {
         put_frame(pt_phys); // Use put_frame on error
         return 0;
    }
    memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
    paging_unmap_range(kernel_page_directory, TEMP_MAP_ADDR_PT, PAGE_SIZE);
    paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
    return (uint32_t*)pt_phys;
}

 
 // Checks if a mapped page table is empty
 static bool is_page_table_empty(uint32_t* pt_virt) {
     if (!pt_virt) return true; // Treat NULL as empty
     for (int i = 0; i < 1024; ++i) {
         if (pt_virt[i] & PAGE_PRESENT) {
             return false;
         }
     }
     return true;
 }