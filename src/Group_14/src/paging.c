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
 
 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL; // Will be set in stage 3
 uint32_t g_kernel_page_directory_phys = 0;     // Ditto
 bool g_pse_supported = false;                  // True if CPU supports 4MB pages
 
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
 
 // --- Forward Declarations ---
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static uint32_t* allocate_page_table_phys_buddy(void);
 static int paging_map_physical(uint32_t *page_directory_phys, uintptr_t phys_addr_to_map, size_t size, uint32_t flags, bool map_to_higher_half);
 
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
  *        and setting the PG bit in CR0.
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
  *        using the buddy allocator. The frame is zeroed before returning.
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
  * @brief Stage 1: Initialize the Page Directory.
  *
  * @param initial_pd_phys A pre-allocated, page-aligned physical address for the PD.
  *                        (Alternatively, you can call paging_alloc_early_frame() to get one.)
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
  *        Establishes identity mappings for the buddy heap and kernel region,
  *        maps the kernel into the higher half, maps VGA memory, and maps the PD itself.
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
  *        Sets global PD pointers, ensures a self-reference mapping, maps
  *        all physical memory into the higher half and activates paging.
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
     terminal_write("  Ensuring Page Directory self-mapping...\n");
     if (paging_map_single((uint32_t*)g_kernel_page_directory_phys, pd_virt_addr, page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
          terminal_write("  [FATAL] Failed to map PD into virtual address space!\n");
          g_kernel_page_directory_phys = 0; 
          g_kernel_page_directory_virt = NULL;
          return -1;
     }
 
     // --- Map ALL Physical Memory to Higher Half ---
     terminal_printf("  Mapping ALL physical memory to higher half [Phys: 0x0 - 0x%x -> Virt: 0x%x - 0x%x)\n",
                     total_memory_bytes, KERNEL_SPACE_VIRT_START, KERNEL_SPACE_VIRT_START + total_memory_bytes);
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
     terminal_printf("  Post-activation read PDE[Self]: 0x%x\n", g_kernel_page_directory_virt[PDE_INDEX(g_kernel_page_directory_virt)]);
 
     terminal_write("[Paging Stage 3] Finalization complete.\n");
     return 0;
 }
 
 // --- Functions Below Operate AFTER Paging is Active ---
 
 /**
  * @brief Helper to allocate and zero a new Page Table frame using the buddy allocator.
  *        The allocated PT is temporarily mapped using the kernel PD virtual pointer.
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
 
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
          terminal_printf("[Paging] Failed to map new PT 0x%x for zeroing!\n", pt_phys);
          BUDDY_FREE(pt_ptr, PAGE_SIZE);
          return NULL;
     }
     memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
     return (uint32_t*)pt_phys;
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
     if (!g_kernel_page_directory_virt) {
         terminal_write("[Paging] map_page_internal: Kernel PD virtual pointer not set.\n");
         return -1;
     }
 
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uintptr_t original_vaddr = vaddr;  // Keep original for TLB invalidation
 
     // Align addresses and set or clear the large page flag accordingly.
     if (use_large_page) {
         if (!g_pse_supported)
             return -1;
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
     int ret = -1;
     bool pt_allocated_here = false;
 
     // Map the page directory temporarily using the kernel PD virtual pointer.
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0)
         return -1;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = target_pd_virt[pd_idx];
 
     // --- Handle Large Page Mapping ---
     if (use_large_page) {
         if (pde & PAGE_PRESENT) {
             goto cleanup_map_page_pd;
         }
         target_pd_virt[pd_idx] = (paddr & 0xFFC00000) | (flags & 0xFFF) | PAGE_SIZE_4MB | PAGE_PRESENT;
         ret = 0;
         tlb_flush_range((void*)vaddr, PAGE_SIZE_LARGE);
         goto cleanup_map_page_pd;
     }
 
     // --- Handle 4KB Page Mapping ---
     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB))
         goto cleanup_map_page_pd;
 
     if (!(pde & PAGE_PRESENT)) {
         pt_phys = (uintptr_t)allocate_page_table_phys_buddy();
         if (pt_phys == 0)
             goto cleanup_map_page_pd;
         pt_allocated_here = true;
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
         if (!(flags & PAGE_USER))
             pde_flags &= ~PAGE_USER;
         target_pd_virt[pd_idx] = (pt_phys & ~0xFFF) | pde_flags;
         pde = target_pd_virt[pd_idx];
         paging_invalidate_page((void*)original_vaddr);
     } else {
         pt_phys = (uintptr_t)(pde & ~0xFFF);
         uint32_t needed_pde_flags = (flags & (PAGE_USER | PAGE_RW));
         if ((pde & needed_pde_flags) != needed_pde_flags) {
             target_pd_virt[pd_idx] |= needed_pde_flags;
             paging_invalidate_page((void*)original_vaddr);
         }
     }
 
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         if (pt_allocated_here) {
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
             target_pd_virt[pd_idx] = 0;
             paging_invalidate_page((void*)original_vaddr);
         }
         goto cleanup_map_page_pd;
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
     uint32_t pt_idx = PTE_INDEX(vaddr);
     if (page_table_virt[pt_idx] & PAGE_PRESENT)
         goto cleanup_map_page_pt;
     page_table_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     ret = 0;
 
 cleanup_map_page_pt:
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
 cleanup_map_page_pd:
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
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
     flags |= PAGE_PRESENT;
     uintptr_t v = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t p = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t end = PAGE_ALIGN_UP(virt_start_addr + memsz);
     while (v < end) {
         bool can_use_large = g_pse_supported &&
                              (v % PAGE_SIZE_LARGE == 0) &&
                              (p % PAGE_SIZE_LARGE == 0) &&
                              ((end - v) >= PAGE_SIZE_LARGE);
         size_t step = can_use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
         if (map_page_internal(page_directory_phys, v, p, flags, can_use_large) != 0) {
             terminal_printf("paging_map_range: Failed mapping page V=0x%x -> P=0x%x (Large=%d)\n", v, p, can_use_large);
             return -1;
         }
         v += step;
         p += step;
     }
     return 0;
 }
 
 /**
  * @brief Check if a page table is empty (i.e. no entry is marked PRESENT).
  * @param pt_virt The virtual address of the page table.
  * @return true if empty, false otherwise.
  */
 static bool is_page_table_empty(uint32_t* pt_virt) {
     if (!pt_virt)
         return true;
     for (int i = 0; i < 1024; ++i) {
         if (pt_virt[i] & PAGE_PRESENT)
             return false;
     }
     return true;
 }
 
 /**
  * @brief Unmap a range of virtual addresses.
  *        Releases the frames (via put_frame or BUDDY_FREE) and clears the mapping.
  *
  * @param page_directory_phys Physical address of the PD.
  * @param virt_start_addr Starting virtual address.
  * @param memsz Size (in bytes) of the range to unmap.
  * @return 0 on success, negative on error.
  */
 int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz) {
     if (!page_directory_phys || memsz == 0)
         return -1;
     if (!g_kernel_page_directory_virt)
         return -1;
 
     uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t virt_end = ALIGN_UP(virt_start_addr + memsz, PAGE_SIZE);
     if (virt_end <= virt_start)
         return -1;
 
     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;
     int final_result = 0;
 
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0)
         return -1;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
 
     for (uintptr_t v = virt_start; v < virt_end; ) {
         uint32_t pd_idx = PDE_INDEX(v);
         uint32_t pde = target_pd_virt[pd_idx];
         if (!(pde & PAGE_PRESENT)) {
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE);
             continue;
         }
         // --- Handle 4MB Page Unmapping ---
         if (pde & PAGE_SIZE_4MB) {
             uintptr_t large_page_v_start = PAGE_LARGE_ALIGN_DOWN(v);
             if (large_page_v_start >= virt_start && (large_page_v_start + PAGE_SIZE_LARGE) <= virt_end) {
                 uintptr_t frame_base_phys = pde & 0xFFC00000;
                 // Release all constituent 4KB frames within the 4MB region.
                 for (int i = 0; i < 1024; ++i) {
                     put_frame(frame_base_phys + i * PAGE_SIZE);
                 }
                 target_pd_virt[pd_idx] = 0;
                 tlb_flush_range((void*)large_page_v_start, PAGE_SIZE_LARGE);
             } else {
                 final_result = -1;
             }
             v = large_page_v_start + PAGE_SIZE_LARGE;
             continue;
         }
         // --- Handle 4KB Page Unmapping ---
         uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF);
         pt_virt = NULL;
         bool pt_was_freed = false;
         if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, pt_phys_val, PTE_KERNEL_DATA_FLAGS) == 0) {
             pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
             uintptr_t pt_range_v_end = PAGE_ALIGN_DOWN(v) + PAGE_SIZE_LARGE;
             uintptr_t loop_end = (virt_end < pt_range_v_end) ? virt_end : pt_range_v_end;
             while (v < loop_end) {
                 uint32_t pt_idx = PTE_INDEX(v);
                 uint32_t pte = pt_virt[pt_idx];
                 if (pte & PAGE_PRESENT) {
                     uintptr_t frame_phys = pte & ~0xFFF;
                     put_frame(frame_phys);
                     pt_virt[pt_idx] = 0;
                     paging_invalidate_page((void*)v);
                 }
                 v += PAGE_SIZE;
             }
             if (is_page_table_empty(pt_virt)) {
                 target_pd_virt[pd_idx] = 0;
                 paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                 paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
                 pt_virt = NULL;
                 pt_was_freed = true;
                 BUDDY_FREE((void*)pt_phys_val, PAGE_SIZE);
             }
             if (pt_virt && !pt_was_freed) {
                 paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PT, PAGE_SIZE);
                 paging_invalidate_page((void*)TEMP_MAP_ADDR_PT);
             }
         } else {
             final_result = -1;
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE);
         }
     } // End for loop over PDEs
 
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
     if (!user && fault_addr < KERNEL_SPACE_VIRT_START) {
         terminal_write("  Error: Kernel fault in user-space!\n");
         goto unhandled_fault;
     }
     if (!g_kernel_page_directory_virt) {
         terminal_write("  Error: Page fault occurred before kernel PD virtual address was set!\n");
         goto unhandled_fault_early;
     }
     if (!current_process || !current_process->mm) {
         terminal_write("  Error: Page fault outside valid process context!\n");
         goto unhandled_fault;
     }
     mm_struct_t *mm = current_process->mm;
     vma_struct_t *vma = find_vma(mm, fault_addr);
     if (!vma) {
         terminal_printf("  Error: No VMA found for addr 0x%x. Segmentation Fault.\n", fault_addr);
         goto unhandled_fault;
     }
     terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);
 
     int result = handle_vma_fault(mm, vma, fault_addr, error_code);
     if (result == 0)
         return;
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
     // Should not reach here.
 
 unhandled_fault_early:
     terminal_write("--- Unhandled Page Fault (Early Init Stage) ---\n");
     terminal_write("Cannot recover. System Halted.\n");
     while (1) { asm volatile("cli; hlt"); }
 }
 
 /**
  * @brief Free user-space mappings.
  *
  * This routine iterates over the PDEs corresponding to user space, frees
  * any page tables (using BUDDY_FREE) and clears the PDE entries.
  *
  * @param page_directory_phys Physical address of the PD.
  */
 void paging_free_user_space(uint32_t *page_directory_phys) {
     if (!page_directory_phys || !g_kernel_page_directory_virt)
         return;
     uint32_t *target_pd_virt = NULL;
     if (paging_map_single(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, (uint32_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0)
         return;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     // Assuming KERNEL_PDE_INDEX defines the first PDE reserved for the kernel.
     for (int i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t pde = target_pd_virt[i];
         if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) {
             uintptr_t pt_phys = pde & ~0xFFF;
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
         }
         target_pd_virt[i] = 0;
     }
     paging_unmap_range(g_kernel_page_directory_virt, TEMP_MAP_ADDR_PD, PAGE_SIZE);
     paging_invalidate_page((void*)TEMP_MAP_ADDR_PD);
 }
 