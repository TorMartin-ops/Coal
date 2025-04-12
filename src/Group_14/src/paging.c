/**
 * paging.c - Paging Implementation with PSE (4MB Pages) support.
 *
 * REVISED (v3 Integrated):
 *   - Fixes recursion in temporary mapping.
 *   - Corrects the use of physical addresses.
 *   - Adds low-level helper functions:
 *         kernel_map_virtual_to_physical_unsafe() and kernel_unmap_virtual_unsafe()
 *     to directly map/unmap kernel pages once paging is active.
 *   - Merges the complete mapping/unmapping logic from the previous (v2) version.
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
 uint32_t* g_kernel_page_directory_virt = NULL; // Set in stage 3
 uint32_t g_kernel_page_directory_phys = 0;     // Ditto
 bool g_pse_supported = false;                  // True if CPU supports 4MB pages
 
 // --- Globals Required (set externally, e.g., in kernel.c) ---
 extern uint32_t g_multiboot_info_phys_addr_global;
 extern uint8_t _kernel_start_phys;
 extern uint8_t _kernel_end_phys;
 
 // Temporary virtual addresses for mapping PD/PT after paging is active.
 // Make sure these addresses are reserved in your kernel layout.
 #ifndef TEMP_MAP_ADDR_PT
 #define TEMP_MAP_ADDR_PT (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PD
 #define TEMP_MAP_ADDR_PD (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
 #endif
 
 // --- Helper Macros ---
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
 
 // Flags for page entries
 #define PAGE_PRESENT   0x001
 #define PAGE_RW        0x002
 #define PAGE_USER      0x004
 #define PAGE_SIZE_4MB  0x080
 
 #define PTE_KERNEL_DATA_FLAGS (PAGE_PRESENT | PAGE_RW)
 #define PTE_USER_DATA_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER)
 
 // Kernel panic for paging errors
 #define PAGING_PANIC(msg) do { \
     terminal_printf("\n[PAGING PANIC] %s. System Halted.\n", msg); \
     while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 
 // --- Early Allocation Tracking ---
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
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type);
 
 // --- New Low-Level Helpers for Temporary Mappings ---
 //
 // These functions assume that paging is active and that g_kernel_page_directory_virt is valid.
 // They provide a direct way to map or unmap a single page into the kernel’s virtual space.
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
     if (!g_kernel_page_directory_virt)
         return -1;
     if (vaddr < KERNEL_SPACE_VIRT_START)
         return -1;
     
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
     
     if (!(pde & PAGE_PRESENT)) {
         terminal_printf("[Kernel Map Unsafe] Error: Kernel PDE[%d] for V=0x%x is not present!\n", pd_idx, vaddr);
         return -1;
     }
     if (pde & PAGE_SIZE_4MB) {
         terminal_printf("[Kernel Map Unsafe] Error: Kernel PDE[%d] for V=0x%x is a 4MB page!\n", pd_idx, vaddr);
         return -1;
     }
     uintptr_t pt_phys = pde & ~0xFFF;
     uintptr_t pt_virt_addr = KERNEL_SPACE_VIRT_START + pt_phys; // Calculate virtual address for PT
     uint32_t *pt_virt = (uint32_t*)pt_virt_addr;
     
     // Write the new PTE directly.
     pt_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     paging_invalidate_page((void*)vaddr);
     return 0;
 }
 
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr) {
     if (!g_kernel_page_directory_virt)
         return;
     if (vaddr < KERNEL_SPACE_VIRT_START)
         return;
     
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];
     
     if (!(pde & PAGE_PRESENT) || (pde & PAGE_SIZE_4MB))
         return;
     
     uintptr_t pt_phys = pde & ~0xFFF;
     uintptr_t pt_virt_addr = KERNEL_SPACE_VIRT_START + pt_phys;
     uint32_t *pt_virt = (uint32_t*)pt_virt_addr;
     
     pt_virt[pt_idx] = 0;
     paging_invalidate_page((void*)vaddr);
 }
 
 // --- Helper to find Multiboot Tag ---
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) return NULL;
     uint32_t total_size = *(uint32_t*)mb_info_phys_addr;
     if (total_size < 8 || total_size > 0x100000) return NULL;
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     uintptr_t info_end = mb_info_phys_addr + total_size;
     while (tag->type != MULTIBOOT_TAG_TYPE_END) {
         uintptr_t current_tag_addr = (uintptr_t)tag;
         if (current_tag_addr + 8 > info_end) return NULL;
         if (tag->size < 8 || (current_tag_addr + tag->size) > info_end) return NULL;
         if (tag->type == type) return tag;
         uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
         if (next_tag_addr >= info_end) break;
         tag = (struct multiboot_tag *)next_tag_addr;
     }
     return NULL;
 }
 
 // --- Early Frame Allocator for Page Tables ---
 
 uintptr_t paging_alloc_early_pt_frame_physical(void) {
     if (g_multiboot_info_phys_addr_global == 0) return 0;
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)
         find_multiboot_tag_early(g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) return 0;
     uintptr_t kernel_start_p = (uintptr_t)&_kernel_start_phys;
     uintptr_t kernel_end_p = (uintptr_t)&_kernel_end_phys;
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
     while ((uintptr_t)mmap_entry < mmap_end) {
         if (mmap_tag->entry_size == 0 || mmap_tag->entry_size < sizeof(multiboot_memory_map_t))
             break;
         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
             uintptr_t region_start = (uintptr_t)mmap_entry->addr;
             uint64_t region_len_64 = mmap_entry->len;
             uintptr_t region_end = region_start + (uintptr_t)region_len_64;
             if (region_end < region_start) region_end = UINTPTR_MAX;
             uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);
             while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr) {
                 if (current_frame_addr < 0x100000) { current_frame_addr += PAGE_SIZE; continue; }
                 bool overlaps_kernel = (current_frame_addr < kernel_end_p &&
                                         (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                 if (overlaps_kernel) { current_frame_addr += PAGE_SIZE; continue; }
                 bool already_allocated = false;
                 for (int i = 0; i < early_allocated_count; ++i) {
                     if (early_allocated_frames[i] == current_frame_addr) { already_allocated = true; break; }
                 }
                 if (already_allocated) { current_frame_addr += PAGE_SIZE; continue; }
                 if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) return 0;
                 early_allocated_frames[early_allocated_count++] = current_frame_addr;
                 memset((void*)current_frame_addr, 0, PAGE_SIZE);
                 return current_frame_addr;
             }
         }
         uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
         if (next_entry_addr > mmap_end) break;
         mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
     }
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
     write_cr4(read_cr4() | (1 << 4));
 }
 
 bool check_and_enable_pse(void) {
     uint32_t eax, ebx, ecx, edx;
     cpuid(1, &eax, &ebx, &ecx, &edx);
     if (edx & (1 << 3)) {
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
 
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }
 
 void paging_invalidate_page(void *vaddr) {
     asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }
 
 void tlb_flush_range(void* start, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr = PAGE_ALIGN_UP((uintptr_t)start + size);
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr);
         addr += PAGE_SIZE;
     }
 }
 
 void paging_activate(uint32_t *page_directory_phys) {
     uint32_t cr0;
     asm volatile("mov %0, %%cr3" : : "r"(page_directory_phys) : "memory");
     asm volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit
     asm volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 
 uintptr_t paging_alloc_early_frame() {
     terminal_write("  [Paging WARNING] Attempting early frame allocation via BUDDY for PD...\n");
     void* frame_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!frame_ptr) {
         terminal_write("  [Paging FATAL] BUDDY_ALLOC failed for initial PD frame!\n");
         return 0;
     }
     uintptr_t frame_phys = (uintptr_t)frame_ptr;
     memset((void*)frame_phys, 0, PAGE_SIZE);
     terminal_printf("  [Paging] Allocated early frame at phys 0x%x\n", frame_phys);
     return frame_phys;
 }
 
 // --- Map Physical Memory (Early) ---
 //
 // This function maps a physical memory range into virtual space (either identity mapped or into higher half)
 // using the early allocator for PTs.
  int paging_map_physical(uint32_t *page_directory_phys, uintptr_t phys_addr_to_map, size_t size, uint32_t flags, bool map_to_higher_half) {
     if (!page_directory_phys || size == 0) return -1;
     uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_to_map);
     uintptr_t end_phys = PAGE_ALIGN_UP(phys_addr_to_map + size);
     flags |= PAGE_PRESENT;
     while (current_phys < end_phys) {
         uintptr_t target_vaddr = map_to_higher_half ? (KERNEL_SPACE_VIRT_START + current_phys) : current_phys;
         uint32_t pd_idx = PDE_INDEX(target_vaddr);
         if (pd_idx >= 1024) return -1;
         uint32_t* pd_entry_ptr = &page_directory_phys[pd_idx];
         uint32_t pde = *pd_entry_ptr;
         uint32_t* pt_phys_ptr = NULL;
         if ((pde & PAGE_PRESENT) && (pde & PAGE_SIZE_4MB))
             return -1; // Conflict: already 4MB page mapped
         if (!(pde & PAGE_PRESENT)) {
             uintptr_t pt_frame_phys = paging_alloc_early_pt_frame_physical();
             if (!pt_frame_phys) return -1;
             pt_phys_ptr = (uint32_t*)pt_frame_phys;
             uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
             if (flags & PAGE_USER) pde_flags |= PAGE_USER;
             *pd_entry_ptr = (pt_frame_phys & ~0xFFF) | pde_flags;
         } else {
             pt_phys_ptr = (uint32_t*)(pde & ~0xFFF);
             uint32_t needed_pde_flags = PAGE_PRESENT | PAGE_RW;
             if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;
             if ((pde & needed_pde_flags) != needed_pde_flags)
                 *pd_entry_ptr |= (flags & (PAGE_RW | PAGE_USER));
         }
         uint32_t pt_idx = PTE_INDEX(target_vaddr);
         if (pt_idx >= 1024) return -1;
         uint32_t* pt_entry_ptr = &pt_phys_ptr[pt_idx];
         *pt_entry_ptr = (current_phys & ~0xFFF) | flags;
         if (current_phys > UINTPTR_MAX - PAGE_SIZE) break;
         current_phys += PAGE_SIZE;
     }
     return 0;
 }
 
 // --- Stage 1: Initialize the Page Directory ---
 uintptr_t paging_initialize_directory(uintptr_t initial_pd_phys) {
     terminal_write("[Paging Stage 1] Configuring Page Directory...\n");
     if (initial_pd_phys == 0 || (initial_pd_phys % PAGE_SIZE != 0))
         PAGING_PANIC("Invalid physical PD address provided!");
     check_and_enable_pse();
     terminal_printf("  [Paging Stage 1] PD structure configured at phys 0x%x.\n", initial_pd_phys);
     return initial_pd_phys;
 }
 
 // --- Stage 2: Setup Early Mappings ---
 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
                             uintptr_t heap_phys_start, size_t heap_size) {
     terminal_write("[Paging Stage 2] Setting up early physical mappings...\n");
     if (page_directory_phys == 0) return -1;
     uint32_t *pd_phys_ptr = (uint32_t*)page_directory_phys;
     
     // Identity map Buddy Heap Region.
     terminal_printf("  Identity mapping Buddy Heap [Phys: 0x%x - 0x%x)\n", heap_phys_start, heap_phys_start + heap_size);
     if (paging_map_physical(pd_phys_ptr, heap_phys_start, heap_size, PTE_KERNEL_DATA_FLAGS, false) != 0)
         PAGING_PANIC("Failed to identity map buddy heap region!");
 
     // Identity map Kernel Region.
     terminal_printf("  Identity mapping Kernel [Phys: 0x%x - 0x%x)\n", kernel_phys_start, kernel_phys_end);
     if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA_FLAGS, false) != 0)
         PAGING_PANIC("Failed to identity map kernel region!");
 
     // Map Kernel into Higher Half.
     terminal_printf("  Mapping Kernel to Higher Half [Phys: 0x%x -> Virt: 0x%x, Size: %u)\n",
                     kernel_phys_start, KERNEL_SPACE_VIRT_START + kernel_phys_start, kernel_phys_end - kernel_phys_start);
     if (paging_map_physical(pd_phys_ptr, kernel_phys_start, kernel_phys_end - kernel_phys_start, PTE_KERNEL_DATA_FLAGS, true) != 0)
         PAGING_PANIC("Failed to map kernel to higher half!");
 
     // Map VGA Memory.
     terminal_write("  Mapping VGA Memory [Phys: 0xB8000 -> Virt: 0xC00B8000)\n");
     if (paging_map_physical(pd_phys_ptr, 0xB8000, PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true) != 0)
         PAGING_PANIC("Failed to map VGA memory!");
 
     // Map the Page Directory into higher half.
     terminal_printf("  Mapping Page Directory self [Phys: 0x%x -> Virt: 0x%x)\n",
                     page_directory_phys, KERNEL_SPACE_VIRT_START + page_directory_phys);
     if (paging_map_physical(pd_phys_ptr, page_directory_phys, PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true) != 0)
         PAGING_PANIC("Failed to map Page Directory to higher half!");
 
     terminal_write("  [Paging Stage 2] Early maps established.\n");
     return 0;
 }
 
 // --- Stage 3: Finalize Mappings and Activate Paging ---
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes) {
     terminal_write("[Paging Stage 3] Finalizing mappings and activating...\n");
     if (page_directory_phys == 0) return -1;
     
     // Set global PD pointers.
     uintptr_t pd_virt_addr = KERNEL_SPACE_VIRT_START + page_directory_phys;
     terminal_printf("  Setting global PD pointers: Phys=0x%x, Virt=0x%x\n", page_directory_phys, pd_virt_addr);
     paging_set_kernel_directory((uint32_t*)pd_virt_addr, page_directory_phys);
     if (g_kernel_page_directory_phys != page_directory_phys ||
         g_kernel_page_directory_virt != (uint32_t*)pd_virt_addr)
         PAGING_PANIC("Failed to set global PD pointers correctly!");
 
     terminal_write("  Ensuring Page Directory self-mapping (already done in Stage 2)...\n");
     terminal_printf("  Mapping ALL physical memory to higher half [Phys: 0x0 - 0x%x -> Virt: 0x%x - 0x%x)\n",
                     total_memory_bytes, KERNEL_SPACE_VIRT_START, KERNEL_SPACE_VIRT_START + total_memory_bytes);
     if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, KERNEL_SPACE_VIRT_START, 0, total_memory_bytes, PTE_KERNEL_DATA_FLAGS) != 0) {
         PAGING_PANIC("Failed to map physical memory to higher half!");
         g_kernel_page_directory_phys = 0; g_kernel_page_directory_virt = NULL;
         return -1;
     }
 
     terminal_write("  Activating Paging...\n");
     paging_activate((uint32_t*)page_directory_phys);
     terminal_write("  [OK] Paging Enabled.\n");
     if (!g_kernel_page_directory_virt)
         PAGING_PANIC("Kernel PD virtual pointer is NULL after activation!");
     terminal_printf("  Post-activation read PDE[Self]: 0x%x\n",
                     g_kernel_page_directory_virt[PDE_INDEX((uintptr_t)g_kernel_page_directory_virt)]);
     terminal_write("[Paging Stage 3] Finalization complete.\n");
     return 0;
 }
 
 // --- Functions That Operate AFTER Paging is Active ---
 
 /**
  * allocate_page_table_phys_buddy() - Allocates and zeroes a new page table frame using the buddy allocator.
  * The new frame is temporarily mapped at TEMP_MAP_ADDR_PT using the new low-level mapping routine.
  */
 static uint32_t* allocate_page_table_phys_buddy(void) {
     void* pt_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!pt_ptr) {
         terminal_write("[Paging] allocate_page_table_phys_buddy: BUDDY_ALLOC failed!\n");
         return NULL;
     }
     uintptr_t pt_phys = (uintptr_t)pt_ptr;
     // Map the new PT frame temporarily using the new low-level mapping.
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         terminal_printf("[Paging] Failed to map new PT 0x%x for zeroing!\n", pt_phys);
         BUDDY_FREE(pt_ptr, PAGE_SIZE);
         return NULL;
     }
     memset((void*)TEMP_MAP_ADDR_PT, 0, PAGE_SIZE);
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT);
     return (uint32_t*)pt_phys;
 }
 
 /**
  * map_page_internal() - Internal helper to map a single page.
  *
  * Supports mapping either a 4KB page or, if conditions allow, a 4MB page.
  * Uses temporary mappings via the low-level kernel_map_virtual_to_physical_unsafe()
  * and kernel_unmap_virtual_unsafe() functions.
  */
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags, bool use_large_page) {
     if (!page_directory_phys || !g_kernel_page_directory_virt) return -1;
 
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uintptr_t original_vaddr = vaddr;
     if (use_large_page) {
         if (!g_pse_supported) return -1;
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
     int map_pd_res = 0;
     int map_pt_res = 0;
 
     // Temporarily map the target page directory.
     map_pd_res = kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS);
     if (map_pd_res != 0) {
         terminal_printf("[Paging] map_page_internal: Failed to temp map target PD. Map result: %d\n", map_pd_res);
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     uint32_t pde = target_pd_virt[pd_idx];
     terminal_printf("  [map_internal] Read target PDE[%d] = 0x%x\n", pd_idx, pde);
 
     if (use_large_page) {
         if (pde & PAGE_PRESENT) {
             terminal_printf("[Paging] map_page_internal: Conflict 4MB over PDE 0x%x V=0x%x\n", pde, vaddr);
             goto cleanup_map_page_pd;
         }
         uint32_t new_pde = (paddr & 0xFFC00000) | (flags & 0xFFF) | PAGE_SIZE_4MB | PAGE_PRESENT;
         terminal_printf("  [map_internal] Writing 4MB PDE[%d]: 0x%x\n", pd_idx, new_pde);
         target_pd_virt[pd_idx] = new_pde;
         ret = 0;
         tlb_flush_range((void*)vaddr, PAGE_SIZE_LARGE);
         goto cleanup_map_page_pd;
     }
 
     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)) {
         terminal_printf("[Paging] map_page_internal: Conflict 4KB over 4MB PDE 0x%x V=0x%x\n", pde, vaddr);
         goto cleanup_map_page_pd;
     }
 
     if (!(pde & PAGE_PRESENT)) {
         terminal_printf("  [map_internal] PDE[%d] not present. Allocating PT...\n", pd_idx);
         pt_phys = (uintptr_t)allocate_page_table_phys_buddy();
         if (pt_phys == 0) {
             terminal_write("[Paging] map_page_internal: Failed to allocate PT frame via buddy.\n");
             goto cleanup_map_page_pd;
         }
         pt_allocated_here = true;
         uint32_t pde_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
         if (!(flags & PAGE_USER))
             pde_flags &= ~PAGE_USER;
         uint32_t new_pde = (pt_phys & ~0xFFF) | pde_flags;
         terminal_printf("  [map_internal] Writing new PDE[%d]: 0x%x (pointing to PT phys 0x%x)\n", pd_idx, new_pde, pt_phys);
         target_pd_virt[pd_idx] = new_pde;
         pde = new_pde;
         paging_invalidate_page((void*)original_vaddr);
     } else {
         pt_phys = (uintptr_t)(pde & ~0xFFF);
         terminal_printf("  [map_internal] PDE[%d] present, points to PT phys 0x%x\n", pd_idx, pt_phys);
         uint32_t needed_pde_flags = PAGE_PRESENT | PAGE_RW;
         if (flags & PAGE_USER) needed_pde_flags |= PAGE_USER;
         if ((pde & needed_pde_flags) != needed_pde_flags) {
             uint32_t promoted_pde = pde | (needed_pde_flags & (PAGE_USER | PAGE_RW));
             terminal_printf("  [map_internal] Promoting PDE[%d] flags from 0x%x to 0x%x\n", pd_idx, pde, promoted_pde);
             target_pd_virt[pd_idx] = promoted_pde;
             paging_invalidate_page((void*)original_vaddr);
             pde = promoted_pde;
         }
     }
 
     // Temporarily map the target page table.
     terminal_printf("  [map_internal] Mapping target PT phys 0x%x to temp virt 0x%x...\n", pt_phys, TEMP_MAP_ADDR_PT);
     map_pt_res = kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT, pt_phys, PTE_KERNEL_DATA_FLAGS);
     if (map_pt_res != 0) {
         terminal_printf("[Paging] map_page_internal: Failed to temp map target PT 0x%x. Map result: %d\n", pt_phys, map_pt_res);
         if (pt_allocated_here) {
             terminal_printf("  [map_internal] Freeing PT frame 0x%x allocated earlier.\n", pt_phys);
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
             target_pd_virt[pd_idx] = 0;
             paging_invalidate_page((void*)original_vaddr);
         }
         goto cleanup_map_page_pd;
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT;
     uint32_t pt_idx = PTE_INDEX(vaddr);
     if (page_table_virt[pt_idx] & PAGE_PRESENT) {
         terminal_printf("[Paging] map_page_internal: Conflict 4KB over existing PTE 0x%x V=0x%x\n", page_table_virt[pt_idx], vaddr);
         goto cleanup_map_page_pt;
     }
     uint32_t new_pte = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     terminal_printf("  [map_internal] Writing PTE[%d] in PT phys 0x%x: 0x%x (V=0x%x -> P=0x%x)\n", pt_idx, pt_phys, new_pte, vaddr, paddr);
     page_table_virt[pt_idx] = new_pte;
     ret = 0;
 
 cleanup_map_page_pt:
     terminal_printf("  [map_internal] Unmapping temp PT virt 0x%x...\n", TEMP_MAP_ADDR_PT);
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT);
     
 cleanup_map_page_pd:
     terminal_printf("  [map_internal] Unmapping temp PD virt 0x%x...\n", TEMP_MAP_ADDR_PD);
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD);
     if (ret == 0) {
         terminal_printf("  [map_internal] Invalidating TLB for V=0x%x\n", original_vaddr);
         paging_invalidate_page((void*)original_vaddr);
     }
     terminal_printf("  [map_internal] Finished for V=0x%x. Result: %d\n", original_vaddr, ret);
     return ret;
 }
 
 /**
  * paging_map_single() - Public API to map a single 4KB page.
  */
 int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
     flags |= PAGE_PRESENT;
     return map_page_internal(page_directory_phys, vaddr, paddr, flags, false);
 }
 
 /**
  * paging_map_range() - Map a range of pages, using large pages where possible.
  */
 int paging_map_range(uint32_t *page_directory_phys, uint32_t virt_start_addr,
                      uint32_t phys_start_addr, uint32_t memsz, uint32_t flags) {
     if (!page_directory_phys || memsz == 0)
         return -1;
     flags |= PAGE_PRESENT;
     uintptr_t v = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t p = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t end = PAGE_ALIGN_UP(virt_start_addr + memsz);
     terminal_printf("[map_range] Mapping V=0x%x-0x%x to P=0x%x-0x%x (Flags=0x%x)\n", v, end, p, p + (end-v), flags);
     while (v < end) {
         bool can_use_large = g_pse_supported &&
                              (v % PAGE_SIZE_LARGE == 0) &&
                              (p % PAGE_SIZE_LARGE == 0) &&
                              ((end - v) >= PAGE_SIZE_LARGE);
         size_t step = can_use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
         terminal_printf("  [map_range] Iter: V=0x%x, P=0x%x, Large=%d, Step=0x%x\n", v, p, can_use_large, step);
         if (map_page_internal(page_directory_phys, v, p, flags, can_use_large) != 0) {
             terminal_printf("paging_map_range: Failed mapping page V=0x%x -> P=0x%x (Large=%d)\n", v, p, can_use_large);
             return -1;
         }
         v += step;
         p += step;
     }
     terminal_printf("[map_range] Completed.\n");
     return 0;
 }
 
 /**
  * is_page_table_empty() - Checks whether a page table has any PRESENT entries.
  */
 static bool is_page_table_empty(uint32_t* pt_virt) {
     if (!pt_virt) return true;
     for (int i = 0; i < 1024; ++i) {
         if (pt_virt[i] & PAGE_PRESENT)
             return false;
     }
     return true;
 }
 
 /**
  * paging_unmap_range() - Unmap a range of virtual addresses.
  */
 int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t memsz) {
     if (!page_directory_phys || memsz == 0 || !g_kernel_page_directory_virt)
         return -1;
     uintptr_t virt_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t virt_end = PAGE_ALIGN_UP(virt_start_addr + memsz);
     if (virt_end <= virt_start)
         return -1;
     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;
     int final_result = 0;
     int map_pd_res = 0;
     int map_pt_res = 0;
     
     map_pd_res = kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS);
     if (map_pd_res != 0) {
         terminal_printf("[Paging] unmap_range: Failed to temp map target PD. Map result: %d\n", map_pd_res);
         return -1;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     
     for (uintptr_t v = virt_start; v < virt_end; ) {
         uint32_t pd_idx = PDE_INDEX(v);
         uint32_t pde = target_pd_virt[pd_idx];
         if (!(pde & PAGE_PRESENT)) {
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE);
             continue;
         }
         if (pde & PAGE_SIZE_4MB) {
             uintptr_t large_page_v_start = PAGE_LARGE_ALIGN_DOWN(v);
             if (large_page_v_start >= virt_start && (large_page_v_start + PAGE_SIZE_LARGE) <= virt_end) {
                 uintptr_t frame_base_phys = pde & 0xFFC00000;
                 for (int i = 0; i < 1024; ++i) {
                     put_frame(frame_base_phys + i * PAGE_SIZE);
                 }
                 target_pd_virt[pd_idx] = 0;
                 tlb_flush_range((void*)large_page_v_start, PAGE_SIZE_LARGE);
             } else {
                 terminal_printf("[Paging] unmap_range: Warning - Partial unmap 4MB V=0x%x requested. Skipping.\n", large_page_v_start);
                 final_result = -1;
             }
             v = large_page_v_start + PAGE_SIZE_LARGE;
             continue;
         }
         uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF);
         pt_virt = NULL;
         bool pt_was_freed = false;
         map_pt_res = kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT, pt_phys_val, PTE_KERNEL_DATA_FLAGS);
         if (map_pt_res == 0) {
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
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT);
                 pt_virt = NULL;
                 pt_was_freed = true;
                 BUDDY_FREE((void*)pt_phys_val, PAGE_SIZE);
             }
             if (pt_virt && !pt_was_freed)
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT);
         } else {
             terminal_printf("[Paging] unmap_range: Failed to temp map PT 0x%x. Map result: %d\n", pt_phys_val, map_pt_res);
             final_result = -1;
             v = ALIGN_UP(v + 1, PAGE_SIZE_LARGE);
         }
     }
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD);
     return final_result;
 }
 
 /**
  * paging_identity_map_range() - Identity map a range.
  */
 int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, uint32_t size, uint32_t flags) {
     return paging_map_range(page_directory_phys, start_addr, start_addr, size, flags | PAGE_PRESENT);
 }
 
 /**
  * page_fault_handler() - Handles page faults.
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
         terminal_write("  Error: Kernel fault accessing address below kernel base!\n");
         goto unhandled_fault;
     }
     if (!g_kernel_page_directory_virt) {
         terminal_write("  Error: Page fault occurred before kernel PD virtual pointer was set!\n");
         goto unhandled_fault_early;
     }
     if (!current_process || !current_process->mm) {
         terminal_write("  Error: Page fault outside valid process context!\n");
         if (!user)
             goto unhandled_fault_early;
         else
             goto unhandled_fault;
     }
     mm_struct_t *mm = current_process->mm;
     vma_struct_t *vma = find_vma(mm, fault_addr);
     if (!vma) {
         terminal_printf("  Error: No VMA found for addr 0x%x. Seg Fault.\n", fault_addr);
         goto unhandled_fault;
     }
     terminal_printf("  Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);
     int result = handle_vma_fault(mm, vma, fault_addr, error_code);
     if (result == 0)
         return;
     else {
         terminal_printf("  Error: handle_vma_fault failed (%d). Seg Fault.\n", result);
         goto unhandled_fault;
     }
     
 unhandled_fault:
     terminal_write("--- Unhandled Page Fault ---\n");
     terminal_printf(" Terminating process (PID %d) fault @ 0x%x.\n", current_pid, fault_addr);
     if (user)
         terminal_printf(" UserESP: 0x%x UserSS: 0x%x\n", regs->user_esp, regs->user_ss);
     terminal_printf("--------------------------\n");
     remove_current_task_with_code(0xFE000000 | error_code);
     
 unhandled_fault_early:
     terminal_write("--- Unhandled Page Fault (Kernel/Early) ---\n");
     PAGING_PANIC("Early or Kernel Page Fault");
 }
 
 /**
  * paging_free_user_space() - Free user-space mappings and associated Page Tables.
  */
 void paging_free_user_space(uint32_t *page_directory_phys) {
     if (!page_directory_phys || !g_kernel_page_directory_virt)
         return;
     uint32_t *target_pd_virt = NULL;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
          terminal_write("[Paging] free_user_space: Failed to temp map target PD.\n");
          return;
     }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD;
     for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t pde = target_pd_virt[i];
         if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) {
             uintptr_t pt_phys = pde & ~0xFFF;
             BUDDY_FREE((void*)pt_phys, PAGE_SIZE);
         }
         target_pd_virt[i] = 0;
     }
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD);
 }
 