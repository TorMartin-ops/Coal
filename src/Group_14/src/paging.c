/**
 * paging.c - Paging Implementation with PSE (4MB Pages) and NX support.
 *
 * REVISED (v5 - Professional Stage 2):
 * - Refactored paging_setup_early_maps for clarity and robustness using struct array.
 * - Added/fixed necessary constants and macro definitions (ARRAY_SIZE).
 * - Ensured type definitions are correctly scoped.
 * - Corrected BUDDY_FREE usage.
 * - Fixed extern declarations and linker symbol integration issues.
 * - Replaced likely unavailable terminal_snprintf with terminal_printf.
 *
 * REVISED (v4 - Major Upgrades):
 * - Added NX (Execute Disable) bit detection and usage.
 * - Added Page Directory Cloning (`paging_clone_directory`).
 * - Added `paging_get_physical_address` query function.
 * - Refined temporary mapping usage.
 *
 * REVISED (v3 Integrated):
 * - Added low-level temporary mapping helpers (kernel_map/unmap_virtual_unsafe).
 */

 #include "paging.h"
 #include "frame.h"              // Needed for put_frame
 #include "buddy.h"              // For BUDDY_ALLOC/BUDDY_FREE macros
 #include "terminal.h"           // For logging, terminal_printf
 #include "types.h"              // For uintptr_t, uint32_t, size_t, bool, etc.
 #include "process.h"            // For pcb_t, mm_struct_t, page_fault_handler context
 #include "mm.h"                 // For VMA definitions (VM_READ/WRITE/EXEC), find_vma, handle_vma_fault
 #include "scheduler.h"          // For remove_current_task_with_code
 #include <string.h>             // For memset
 #include "cpuid.h"              // For cpuid wrapper
 #include "kmalloc_internal.h"   // For ALIGN_UP/DOWN macros (ensure defined)
 #include "multiboot2.h"         // Needed for early frame allocation
 #include "msr.h"                // For rdmsr/wrmsr definitions
 // #include "utils.h"           // Include if ARRAY_SIZE is defined there
 
 // --- Ensure Base Macros/Constants are Defined ---
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096
 #endif
 #ifndef PAGE_SIZE_LARGE
 #define PAGE_SIZE_LARGE (4 * 1024 * 1024) // 4 MiB
 #endif
 #define PAGES_PER_TABLE 1024
 #define TABLES_PER_DIR  1024
 
 // Indices
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
 #define PAGE_OFFSET(addr) ((uintptr_t)(addr) & 0xFFF)
 
 // Alignment Macros (ensure defined, e.g., via kmalloc_internal.h or types.h)
 #ifndef PAGE_ALIGN_DOWN
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
 #endif
 #ifndef PAGE_ALIGN_UP
 #define PAGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
 #endif
 #ifndef PAGE_LARGE_ALIGN_DOWN
 #define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
 #endif
 #ifndef PAGE_LARGE_ALIGN_UP
 #define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))
 #endif
 
 // Page Table/Directory Entry Flags
 #define PAGE_PRESENT  0x001 // Present bit
 #define PAGE_RW       0x002 // Read/Write bit
 #define PAGE_USER     0x004 // User/Supervisor bit
 #define PAGE_PWT      0x008 // Page Write-Through
 #define PAGE_PCD      0x010 // Page Cache Disable
 #define PAGE_ACCESSED 0x020 // Accessed bit
 #define PAGE_DIRTY    0x040 // Dirty bit (PTE only)
 #define PAGE_SIZE_4MB 0x080 // Page Size bit (PDE only)
 #define PAGE_GLOBAL   0x100 // Global Page bit (PTE only, requires CR4.PGE)
 #define PAGE_NX       (1ULL << 63) // No-Execute bit (EFER.NXE must be set)
 #define PAGING_DEBUG 1
 
 // Combined Flags (Ensure these match your desired permissions)
 #ifndef PTE_KERNEL_DATA_FLAGS
 #define PTE_KERNEL_DATA_FLAGS (PAGE_PRESENT | PAGE_RW | PAGE_NX) // RW, Not Executable
 #endif
 #ifndef PTE_KERNEL_CODE_FLAGS
 #define PTE_KERNEL_CODE_FLAGS (PAGE_PRESENT | PAGE_RW)          // RW, Executable
 #endif
 #ifndef PTE_KERNEL_READONLY_NX_FLAGS
 #define PTE_KERNEL_READONLY_NX_FLAGS (PAGE_PRESENT | PAGE_NX) // Read-Only, Not Executable
 #endif
 #ifndef PTE_USER_DATA_FLAGS
 #define PTE_USER_DATA_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX) // User RW-NX
 #endif
 #ifndef PTE_USER_CODE_FLAGS
 #define PTE_USER_CODE_FLAGS   (PAGE_PRESENT | PAGE_RW | PAGE_USER)          // User RWX
 #endif
 // Helper macro to get PDE flags for a PT based on desired PTE permissions
 #define PDE_FLAGS_FROM_PTE(pte_flags) ((pte_flags) & (PAGE_PRESENT | PAGE_RW | PAGE_USER))
 
 // Kernel Space Definition
 #ifndef KERNEL_SPACE_VIRT_START
 #define KERNEL_SPACE_VIRT_START 0xC0000000
 #endif
 // Calculate index of first kernel PDE (useful boundary)
 #define KERNEL_PDE_INDEX PDE_INDEX(KERNEL_SPACE_VIRT_START)
 
 // Other Constants
 #ifndef VGA_PHYS_ADDR
 #define VGA_PHYS_ADDR 0xB8000
 #endif
 
 // Temporary mapping addresses (ensure these are reserved!)
 #ifndef TEMP_MAP_ADDR_PT_DST
 #define TEMP_MAP_ADDR_PT_DST (KERNEL_SPACE_VIRT_START - 4 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PD_DST
 #define TEMP_MAP_ADDR_PD_DST (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PT_SRC
 #define TEMP_MAP_ADDR_PT_SRC (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
 #endif
 #ifndef TEMP_MAP_ADDR_PD_SRC
 #define TEMP_MAP_ADDR_PD_SRC (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
 #endif
 
 // Panic Macro
 #ifndef PAGING_PANIC
 #define PAGING_PANIC(msg) do { \
     terminal_printf("\n[PAGING PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
     while (1) { asm volatile("cli; hlt"); } \
 } while(0)
 #endif
 
 // Utility Macro
 #ifndef ARRAY_SIZE
 #define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
 #endif
 
 
 // --- Globals ---
 uint32_t* g_kernel_page_directory_virt = NULL; // Virtual address of kernel PD
 uint32_t g_kernel_page_directory_phys = 0;     // Physical address of kernel PD
 bool g_pse_supported = false;                  // 4MB page support
 bool g_nx_supported = false;                   // No-Execute support
 
 // --- Required External Symbols ---
 // Linker Script Symbols
 extern uint8_t _kernel_start_phys;
 extern uint8_t _kernel_end_phys;
 extern uint8_t _kernel_text_start_phys;
 extern uint8_t _kernel_text_end_phys;
 extern uint8_t _kernel_rodata_start_phys;
 extern uint8_t _kernel_rodata_end_phys;
 extern uint8_t _kernel_data_start_phys;
 extern uint8_t _kernel_data_end_phys;
 // Multiboot Info
 extern uint32_t g_multiboot_info_phys_addr_global;
 
 // --- Early Allocation Tracking ---
 #define MAX_EARLY_ALLOCATIONS 128
 static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
 static int early_allocated_count = 0;
 
 // --- Forward Declarations ---
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint64_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static bool check_and_enable_nx(void);
 static uint32_t* allocate_page_table_phys(void);
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type);
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint64_t flags);
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr);
 uintptr_t paging_alloc_early_pt_frame_physical(void); // Needs definition if not here
 int paging_map_physical_early(uint32_t *page_directory_phys, uintptr_t phys_addr_start, size_t size, uint64_t flags, bool map_to_higher_half); // Needs definition if not here
 
 
 // --- Helper Structure for Defining Memory Regions to Map ---
 // Moved definition outside and before paging_setup_early_maps
 typedef struct {
     const char* name;           // Descriptive name for logging/errors
     uintptr_t phys_start;       // Physical start address
     uintptr_t phys_end;         // Physical end address (exclusive)
     uint64_t flags;             // PTE flags for the mapping
     bool map_higher_half;       // Map identity (false) or higher-half (true)
     bool required;              // If true, panic if mapping fails or size is zero
 } memory_region_map_t;
 
 
 // --- Low-Level Helpers for Temporary Mappings (Post-Paging) ---
 // Definition copied from previous version... ensure correct
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint64_t flags) {
      if (!g_kernel_page_directory_virt) {
           terminal_printf("[Kernel Map Unsafe] Error: Kernel PD not set!\n");
           return -1;
      }
      if (vaddr < KERNEL_SPACE_VIRT_START || vaddr >= (UINTPTR_MAX - PAGE_SIZE + 1)) {
           terminal_printf("[Kernel Map Unsafe] Error: Invalid temporary V=0x%x!\n", vaddr);
          return -1;
      }
      if (vaddr % PAGE_SIZE != 0 || paddr % PAGE_SIZE != 0) {
           terminal_printf("[Kernel Map Unsafe] Error: Unaligned V=0x%x or P=0x%x!\n", vaddr, paddr);
          return -1;
      }
 
      uint32_t pd_idx = PDE_INDEX(vaddr);
      uint32_t pt_idx = PTE_INDEX(vaddr);
      uint32_t pde = g_kernel_page_directory_virt[pd_idx];
 
      if (!(pde & PAGE_PRESENT)) {
          terminal_printf("[Kernel Map Unsafe] Error: Kernel PDE[%d] for temp V=0x%x is not present! PDE=0x%x\n", pd_idx, vaddr, pde);
          PAGING_PANIC("Missing kernel page table for temporary mapping area");
          return -1;
      }
      if (pde & PAGE_SIZE_4MB) {
          terminal_printf("[Kernel Map Unsafe] Error: Kernel PDE[%d] for temp V=0x%x is a 4MB page! Cannot use for temp.\n", pd_idx, vaddr);
          PAGING_PANIC("Kernel temporary mapping area covered by 4MB page");
          return -1;
      }
 
      // Assuming kernel physical memory [0, TotalMem) is mapped at [KERNEL_SPACE_VIRT_START, KERNEL_SPACE_VIRT_START + TotalMem)
      uintptr_t pt_phys = pde & ~0xFFF;
      uintptr_t pt_virt_addr = KERNEL_SPACE_VIRT_START + pt_phys; // *ASSUMPTION*
      uint32_t *pt_virt = (uint32_t*)pt_virt_addr;
 
      // Check if the temporary PTE slot is already in use
      if (pt_virt[pt_idx] & PAGE_PRESENT) {
           terminal_printf("[Kernel Map Unsafe] Warning: Temp V=0x%x PTE[%d] already present (0x%x)! Overwriting.\n", vaddr, pt_idx, pt_virt[pt_idx]);
           pt_virt[pt_idx] = 0; // Overwrite requires clearing first
           paging_invalidate_page((void*)vaddr);
      }
 
      uint32_t pte_flags_32 = (flags & 0xFFF); // Get lower 12 bits
      // Note: NX bit handling for 32-bit non-PAE is ignored here by hardware
      pt_virt[pt_idx] = (paddr & ~0xFFF) | pte_flags_32 | PAGE_PRESENT;
 
      paging_invalidate_page((void*)vaddr);
      return 0;
 }
 
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr) {
      if (!g_kernel_page_directory_virt) return;
      if (vaddr < KERNEL_SPACE_VIRT_START || vaddr >= (UINTPTR_MAX - PAGE_SIZE + 1)) return;
 
      uint32_t pd_idx = PDE_INDEX(vaddr);
      uint32_t pt_idx = PTE_INDEX(vaddr);
      uint32_t pde = g_kernel_page_directory_virt[pd_idx];
 
      if (!(pde & PAGE_PRESENT) || (pde & PAGE_SIZE_4MB)) {
          return;
      }
 
      uintptr_t pt_phys = pde & ~0xFFF;
      uintptr_t pt_virt_addr = KERNEL_SPACE_VIRT_START + pt_phys; // *ASSUMPTION*
      uint32_t *pt_virt = (uint32_t*)pt_virt_addr;
 
      if (pt_virt[pt_idx] & PAGE_PRESENT) {
          pt_virt[pt_idx] = 0;
          paging_invalidate_page((void*)vaddr);
      }
 }
 
 // --- Helper to find Multiboot Tag ---
 // Definition copied from previous version... ensure correct
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) return NULL;
     uint32_t total_size = *(volatile uint32_t*)mb_info_phys_addr;
     if (total_size < 8 || total_size > 0x10000) return NULL;
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     uintptr_t info_end = mb_info_phys_addr + total_size;
     while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
         uintptr_t current_tag_addr = (uintptr_t)tag;
         if (current_tag_addr + sizeof(struct multiboot_tag) > info_end) return NULL;
         if (tag->size < 8) return NULL;
         if (current_tag_addr + tag->size > info_end) return NULL;
         if (tag->type == type) return tag;
         uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
         if (next_tag_addr >= info_end) break;
         tag = (struct multiboot_tag *)next_tag_addr;
     }
     return NULL;
 }
 
 // --- Early Frame Allocator ---
 // Definition copied from previous version... ensure correct and includes memset
 uintptr_t paging_alloc_early_pt_frame_physical(void) {
     if (g_multiboot_info_phys_addr_global == 0) {
         terminal_write("[Paging Early Alloc] Multiboot info address not set!\n");
         return 0;
     }
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)
         find_multiboot_tag_early(g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) {
         terminal_write("[Paging Early Alloc] Multiboot MMAP tag not found!\n");
         return 0;
     }
     uintptr_t kernel_start_p = (uintptr_t)&_kernel_start_phys;
     uintptr_t kernel_end_p = PAGE_ALIGN_UP((uintptr_t)&_kernel_end_phys);
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
     while ((uintptr_t)mmap_entry < mmap_end) {
          if (mmap_tag->entry_size == 0 || (uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end) {
               terminal_write("[Paging Early Alloc] Invalid MMAP entry size or bounds.\n");
               break;
          }
         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
             uintptr_t region_start = (uintptr_t)mmap_entry->addr;
             uint64_t region_len_64 = mmap_entry->len;
             uintptr_t region_end = region_start + (uintptr_t)region_len_64;
             if (region_end < region_start) region_end = UINTPTR_MAX;
             uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);
             while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr) {
                 if (current_frame_addr < 0x100000) { current_frame_addr += PAGE_SIZE; continue; }
                 bool overlaps_kernel = (current_frame_addr < kernel_end_p && (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                 if (overlaps_kernel) { current_frame_addr += PAGE_SIZE; continue; }
                 uint32_t mb_info_size = *(volatile uint32_t*)g_multiboot_info_phys_addr_global; if (mb_info_size < 8) mb_info_size = 8;
                 uintptr_t mb_info_end = g_multiboot_info_phys_addr_global + mb_info_size;
                 bool overlaps_mb_info = (current_frame_addr < mb_info_end && (current_frame_addr + PAGE_SIZE) > g_multiboot_info_phys_addr_global);
                 if (overlaps_mb_info) { current_frame_addr += PAGE_SIZE; continue; }
                 bool already_allocated = false;
                 for (int i = 0; i < early_allocated_count; ++i) {
                     if (early_allocated_frames[i] == current_frame_addr) { already_allocated = true; break; }
                 }
                 if (already_allocated) { current_frame_addr += PAGE_SIZE; continue; }
                 if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) {
                     terminal_write("[Paging Early Alloc] Exceeded MAX_EARLY_ALLOCATIONS!\n");
                     return 0;
                 }
                 early_allocated_frames[early_allocated_count++] = current_frame_addr;
                 // *** Ensure frame is zeroed ***
                 memset((void*)current_frame_addr, 0, PAGE_SIZE);
                 return current_frame_addr;
             }
         }
         uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
         if (next_entry_addr > mmap_end || next_entry_addr < (uintptr_t)mmap_entry) break;
         mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
     }
     terminal_write("[Paging Early Alloc] No suitable frame found in memory map!\n");
     return 0;
 }
 
 // --- CPU Feature Detection and Control ---
 static inline uint32_t read_cr4(void) { uint32_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
 static inline void write_cr4(uint32_t v) { asm volatile("mov %0, %%cr4" :: "r"(v)); }
 static inline void enable_cr4_pse(void) { write_cr4(read_cr4() | (1 << 4)); }
 
 bool check_and_enable_pse(void) { /* Definition from previous version */
     uint32_t eax, ebx, ecx, edx;
     cpuid(1, &eax, &ebx, &ecx, &edx);
     if (edx & (1 << 3)) {
         terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
         enable_cr4_pse();
         if (read_cr4() & (1 << 4)) {
             terminal_write("[Paging] CR4.PSE bit enabled.\n");
             g_pse_supported = true; return true;
         } else {
             terminal_write("[Paging Error] Failed to enable CR4.PSE bit!\n");
             g_pse_supported = false; return false;
         }
     } else {
         terminal_write("[Paging] CPU does not support PSE (4MB Pages).\n");
         g_pse_supported = false; return false;
     }
 }
 
 static bool check_and_enable_nx(void) { /* Definition from previous version */
     uint32_t eax, ebx, ecx, edx;
     cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
     if (eax < 0x80000001) {
         terminal_write("[Paging] CPUID leaf 0x80000001 not supported. Cannot check NX.\n");
         g_nx_supported = false; return false;
     }
     cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
     if (edx & (1 << 20)) {
         terminal_write("[Paging] CPU supports NX (Execute Disable) bit.\n");
         uint64_t efer = rdmsr(MSR_EFER); efer |= (1ULL << 11); wrmsr(MSR_EFER, efer);
         efer = rdmsr(MSR_EFER);
         if (efer & (1ULL << 11)) {
              terminal_write("[Paging] EFER.NXE bit enabled.\n");
              g_nx_supported = true; return true;
         } else {
              terminal_write("[Paging Error] Failed to enable EFER.NXE bit!\n");
              g_nx_supported = false; return false;
         }
     } else {
         terminal_write("[Paging] CPU does not support NX bit.\n");
         g_nx_supported = false; return false;
     }
 }
 
 // --- Public API Functions ---
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) { /* Definition from previous version */
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
 }
 
 void paging_invalidate_page(void *vaddr) { /* Definition from previous version */
     asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
 }
 
 void tlb_flush_range(void* start, size_t size) { /* Definition from previous version */
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr = PAGE_ALIGN_UP((uintptr_t)start + size);
     if (end_addr < addr) end_addr = UINTPTR_MAX;
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr);
         if (addr > UINTPTR_MAX - PAGE_SIZE) break;
         addr += PAGE_SIZE;
     }
 }
 
 void paging_activate(uint32_t *page_directory_phys) { /* Definition from previous version */
     uint32_t cr0;
     uintptr_t pd_phys_addr = (uintptr_t)page_directory_phys;
     asm volatile("mov %0, %%cr3" : : "r"(pd_phys_addr) : "memory");
     asm volatile("mov %%cr0, %0" : "=r"(cr0));
     cr0 |= 0x80000000; // Set PG bit
     asm volatile("mov %0, %%cr0" : : "r"(cr0));
 }
 
 // --- Buddy Frame Allocator (Post-Init) ---
 uintptr_t paging_alloc_frame_buddy(void) { /* Definition from previous version - ensure BUDDY_FREE is correct */
     void* frame_ptr = BUDDY_ALLOC(PAGE_SIZE);
     if (!frame_ptr) {
         terminal_write("[Paging] BUDDY_ALLOC failed for frame!\n");
         return 0;
     }
     // *** Adjust based on whether BUDDY_ALLOC returns virt or phys ***
     uintptr_t frame_phys = (uintptr_t)frame_ptr; // ASSUMPTION: returns phys or identity mapped virt
 
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, frame_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
          terminal_printf("[Paging] Failed map frame 0x%x for zeroing!\n", frame_phys);
          BUDDY_FREE(frame_ptr); // Corrected: Pass only pointer
          return 0;
     }
     memset((void*)TEMP_MAP_ADDR_PT_DST, 0, PAGE_SIZE);
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
     return frame_phys;
 }
 
 static uint32_t* allocate_page_table_phys(void) { /* Definition from previous version */
     uintptr_t pt_phys = paging_alloc_frame_buddy();
     if (!pt_phys) {
          terminal_write("[Paging] allocate_page_table_phys: Failed to allocate frame via buddy.\n");
          return NULL;
     }
     return (uint32_t*)pt_phys;
 }
 
 // --- Early Mapping Function ---
 // Definition copied from previous version... ensure correct
 int paging_map_physical_early(uint32_t *page_directory_phys, uintptr_t phys_addr_start, size_t size, uint64_t flags, bool map_to_higher_half) {
     if (!page_directory_phys || size == 0) { return -1; }
     uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_start);
     uintptr_t end_phys;
     if (phys_addr_start > UINTPTR_MAX - size) { end_phys = UINTPTR_MAX; } else { end_phys = phys_addr_start + size; }
     uintptr_t aligned_end_phys = PAGE_ALIGN_UP(end_phys); if (aligned_end_phys < end_phys) { aligned_end_phys = UINTPTR_MAX; } end_phys = aligned_end_phys;
     if (end_phys <= current_phys) { return -1; }
     uint32_t pte_flags_32 = (flags & (PAGE_RW | PAGE_USER)) | PAGE_PRESENT;
     while (current_phys < end_phys) {
         uintptr_t target_vaddr;
         if (map_to_higher_half) {
             if (current_phys > UINTPTR_MAX - KERNEL_SPACE_VIRT_START) { return -1; }
             target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
         } else { target_vaddr = current_phys; }
         uint32_t pd_idx = PDE_INDEX(target_vaddr); if (pd_idx >= 1024) { return -1; }
         uint32_t* pd_entry_ptr = &page_directory_phys[pd_idx]; uint32_t pde = *pd_entry_ptr;
         uintptr_t pt_phys_addr;
         if (!(pde & PAGE_PRESENT)) {
             pt_phys_addr = paging_alloc_early_pt_frame_physical(); if (!pt_phys_addr) { return -1; }
             uint32_t pde_flags = PDE_FLAGS_FROM_PTE(pte_flags_32);
             *pd_entry_ptr = (pt_phys_addr & ~0xFFF) | pde_flags;
         } else {
             if (pde & PAGE_SIZE_4MB) { terminal_printf("[Early Map] Error: Conflict at V=0x%x - PDE[%d] is 4MB!\n", target_vaddr, pd_idx); return -1; }
             pt_phys_addr = (uintptr_t)(pde & ~0xFFF);
             uint32_t needed_pde_flags = PDE_FLAGS_FROM_PTE(pte_flags_32);
             if ((pde & needed_pde_flags) != needed_pde_flags) { *pd_entry_ptr |= (needed_pde_flags & (PAGE_RW | PAGE_USER)); }
         }
         uint32_t* pt_phys_ptr = (uint32_t*)pt_phys_addr; uint32_t pt_idx = PTE_INDEX(target_vaddr); if (pt_idx >= 1024) { return -1; }
         uint32_t* pt_entry_ptr = &pt_phys_ptr[pt_idx];
         if (*pt_entry_ptr & PAGE_PRESENT) {
              terminal_printf("[Early Map] Error: Conflict at V=0x%x - PTE[%d] already present (0x%x)!\n", target_vaddr, pt_idx, *pt_entry_ptr);
              return -1; // Return error on conflict
         }
         *pt_entry_ptr = (current_phys & ~0xFFF) | pte_flags_32;
         if (current_phys > UINTPTR_MAX - PAGE_SIZE) break;
         current_phys += PAGE_SIZE;
     }
     return 0;
 }
 
 // --- Paging Initialization Stages ---
 
 int paging_initialize_directory(uintptr_t* initial_pd_phys) { /* Definition from previous version */
     terminal_write("[Paging Stage 1] Initializing Page Directory...\n");
     uintptr_t pd_phys = paging_alloc_early_pt_frame_physical();
     if (!pd_phys) { PAGING_PANIC("Failed to allocate frame for PD!"); return -1; }
     terminal_printf("  [Paging Stage 1] Allocated PD at physical address 0x%x.\n", pd_phys);
     check_and_enable_pse(); check_and_enable_nx();
     *initial_pd_phys = pd_phys;
     terminal_write("[Paging Stage 1] Directory allocated and features checked.\n");
     return 0;
 }
 
 
 /**
  * @brief Stage 2: Sets up essential early mappings needed before paging is activated.
  * Defines essential regions (heap, kernel sections, VGA, PD) and maps them
  * using the early mapper function `paging_map_physical_early`.
  * Professional version focusing on clarity, robustness, and maintainability.
  *
  * @param page_directory_phys Physical address of the page directory.
  * @param kernel_phys_start Unused (sections defined by linker symbols).
  * @param kernel_phys_end Unused.
  * @param heap_phys_start Physical start address of the early heap (buddy).
  * @param heap_size Size of the early heap.
  * @return 0 on success, calls PAGING_PANIC on failure.
  */
 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start __attribute__((unused)),
                             uintptr_t kernel_phys_end __attribute__((unused)),
                             uintptr_t heap_phys_start, size_t heap_size)
 {
     terminal_write("[Paging Stage 2] Setting up early physical mappings...\n");
 
     // --- Basic Validation ---
     if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) {
         PAGING_PANIC("Stage 2: Invalid page directory physical address provided!");
         return -1; // Unreachable
     }
     uint32_t *pd_phys_ptr = (uint32_t*)page_directory_phys;
 
     // --- Calculate Aligned Section Boundaries from Linker Symbols ---
     uintptr_t text_start   = PAGE_ALIGN_DOWN((uintptr_t)&_kernel_text_start_phys);
     uintptr_t text_end     = PAGE_ALIGN_UP((uintptr_t)&_kernel_text_end_phys);
     uintptr_t rodata_start = PAGE_ALIGN_DOWN((uintptr_t)&_kernel_rodata_start_phys);
     uintptr_t rodata_end   = PAGE_ALIGN_UP((uintptr_t)&_kernel_rodata_end_phys);
     uintptr_t data_start   = PAGE_ALIGN_DOWN((uintptr_t)&_kernel_data_start_phys);
     uintptr_t data_end     = PAGE_ALIGN_UP((uintptr_t)&_kernel_data_end_phys);
 
     // Calculate aligned heap boundaries
     uintptr_t heap_start   = PAGE_ALIGN_DOWN(heap_phys_start);
     uintptr_t heap_end     = PAGE_ALIGN_UP(heap_phys_start + heap_size);
     if (heap_end <= heap_start) { heap_end = heap_start; } // Avoid zero/negative size
 
     // --- Define Memory Regions to Map ---
     memory_region_map_t regions_to_map[] = {
         {"Buddy Heap",   heap_start,   heap_end,     PTE_KERNEL_DATA_FLAGS, false, false},
         {".text",        text_start,   text_end,     PTE_KERNEL_CODE_FLAGS, true,  true},
         {".rodata",      rodata_start, rodata_end,   PTE_KERNEL_READONLY_NX_FLAGS, true, true},
         {".data/.bss",   data_start,   data_end,     PTE_KERNEL_DATA_FLAGS, true,  true},
         {"VGA",          VGA_PHYS_ADDR, VGA_PHYS_ADDR + PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true, true},
         {"PD Self-Map",  page_directory_phys, page_directory_phys + PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true, true},
     };
 
     // --- Perform Mappings ---
     for (size_t i = 0; i < ARRAY_SIZE(regions_to_map); ++i) {
         memory_region_map_t* region = &regions_to_map[i];
         size_t size = (region->phys_end > region->phys_start) ? (region->phys_end - region->phys_start) : 0;
 
         if (size == 0) {
             if (region->required) {
                 terminal_printf("KERNEL PANIC: Required region '%s' has zero size!\n", region->name ? region->name : "<NULL>");
                 PAGING_PANIC("Zero size for required mapping region");
                 return -1; // Unreachable
             }
             #ifdef PAGING_DEBUG
             terminal_printf("  Skipping mapping for zero-size region: %s\n", region->name ? region->name : "<NULL>");
             #endif
             continue;
         }
 
         #ifdef PAGING_DEBUG
         uintptr_t target_vaddr_start = region->map_higher_half ? (KERNEL_SPACE_VIRT_START + region->phys_start) : region->phys_start;
         terminal_printf("  DEBUG: Trying map %-12s: P=[0x%x-0x%x) Size=%u KB -> V=0x%x Higher=%d Flags=0x%llx\n",
                         region->name ? region->name : "<NULL>", region->phys_start, region->phys_end, size / 1024,
                         target_vaddr_start, region->map_higher_half, region->flags);
         #endif
 
         int result = paging_map_physical_early(pd_phys_ptr, region->phys_start, size, region->flags, region->map_higher_half);
 
         if (result != 0) {
              terminal_printf("KERNEL PANIC: Failed to map '%s' region! Error in paging_map_physical_early.\n", region->name ? region->name : "<NULL>");
              PAGING_PANIC("Failed to map essential early region");
              return -1; // Unreachable
         }
     }
 
     terminal_write("[Paging Stage 2] Early maps established successfully.\n");
     return 0;
 }
 
 
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes) { /* Definition from previous version */
     terminal_write("[Paging Stage 3] Finalizing mappings and activating...\n");
     if (page_directory_phys == 0) return -1;
     uintptr_t pd_virt_addr = KERNEL_SPACE_VIRT_START + page_directory_phys;
     terminal_printf("  Setting global PD pointers: Phys=0x%x, Virt=0x%x\n", page_directory_phys, pd_virt_addr);
     paging_set_kernel_directory((uint32_t*)pd_virt_addr, page_directory_phys);
     if (g_kernel_page_directory_phys != page_directory_phys || g_kernel_page_directory_virt != (uint32_t*)pd_virt_addr) { PAGING_PANIC("Failed to set global PD pointers!"); return -1; }
     uintptr_t map_size = total_memory_bytes;
     const uintptr_t max_mappable_phys_addr = 0xFFFFF000;
     if (total_memory_bytes == 0) { map_size = 0; } else if (total_memory_bytes >= max_mappable_phys_addr) { map_size = max_mappable_phys_addr; }
     if (map_size > 0) {
         uintptr_t phys_map_virt_end = KERNEL_SPACE_VIRT_START + map_size;
         if (phys_map_virt_end < KERNEL_SPACE_VIRT_START) { map_size = PAGE_ALIGN_DOWN(UINTPTR_MAX - KERNEL_SPACE_VIRT_START + 1); if (map_size == 0) { PAGING_PANIC("Cannot map phys mem"); return -1; } }
         terminal_printf("  Mapping Physical Memory to Higher Half [Phys: 0x0 - 0x%x) -> Virt: 0x%x - 0x%x) RW-NX\n", map_size, KERNEL_SPACE_VIRT_START, KERNEL_SPACE_VIRT_START + map_size);
         if (paging_map_range((uint32_t*)g_kernel_page_directory_phys, KERNEL_SPACE_VIRT_START, 0, map_size, PTE_KERNEL_DATA_FLAGS) != 0) { PAGING_PANIC("Failed to map physical memory to higher half!"); return -1; }
     }
     terminal_write("  Activating Paging...\n");
     paging_activate((uint32_t*)page_directory_phys);
     terminal_write("  [OK] Paging Enabled.\n");
     if (!g_kernel_page_directory_virt) { PAGING_PANIC("Kernel PD virtual pointer is NULL after activation!"); }
     uint32_t test_pde_after = g_kernel_page_directory_virt[PDE_INDEX((uintptr_t)g_kernel_page_directory_virt)];
     if ((test_pde_after & ~0xFFF) != page_directory_phys) { PAGING_PANIC("PD self-mapping check failed after activation!"); }
     terminal_write("[Paging Stage 3] Finalization complete.\n");
     return 0;
 }
 
 
 // --- Functions That Operate AFTER Paging is Active ---
 
 static int map_page_internal(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint64_t flags, bool use_large_page) { /* Definition from previous version - ensure BUDDY_FREE takes 1 arg */
     if (!g_kernel_page_directory_virt) { return -1; } if (!page_directory_phys) { return -1; }
     uintptr_t original_vaddr = vaddr; uintptr_t aligned_vaddr; uintptr_t aligned_paddr;
     uint32_t pde_final_flags; uint32_t pte_final_flags;
     if (use_large_page) {
         if (!g_pse_supported) { return -1; }
         aligned_vaddr = PAGE_LARGE_ALIGN_DOWN(vaddr); aligned_paddr = PAGE_LARGE_ALIGN_DOWN(paddr);
         pde_final_flags = (flags & (PAGE_RW | PAGE_USER)) | PAGE_PRESENT | PAGE_SIZE_4MB;
         if (g_nx_supported && (flags & PAGE_NX)) { /* No-op for PDE */ }
         pde_final_flags &= ~(PAGE_DIRTY | PAGE_ACCESSED | PAGE_GLOBAL);
     } else {
         aligned_vaddr = PAGE_ALIGN_DOWN(vaddr); aligned_paddr = PAGE_ALIGN_DOWN(paddr);
         pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_GLOBAL | PAGE_DIRTY | PAGE_ACCESSED)) | PAGE_PRESENT;
         if (g_nx_supported && (flags & PAGE_NX)) { /* No-op for PTE */ }
         pde_final_flags = PDE_FLAGS_FROM_PTE(pte_final_flags);
     }
     uint32_t pd_idx = PDE_INDEX(aligned_vaddr); uint32_t* target_pd_virt = NULL; uint32_t* page_table_virt = NULL;
     uintptr_t pt_phys = 0; int ret = -1; bool pt_allocated_here = false;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) { return -1; }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST; uint32_t pde = target_pd_virt[pd_idx];
     if (use_large_page) {
         if (pde & PAGE_PRESENT) { goto cleanup_map_page_pd; }
         uint32_t new_pde = (aligned_paddr & 0xFFC00000) | pde_final_flags;
         target_pd_virt[pd_idx] = new_pde; ret = 0; tlb_flush_range((void*)original_vaddr, PAGE_SIZE_LARGE); goto cleanup_map_page_pd;
     }
     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)) { goto cleanup_map_page_pd; }
     if (!(pde & PAGE_PRESENT)) {
         pt_phys = (uintptr_t)allocate_page_table_phys(); if (pt_phys == 0) { goto cleanup_map_page_pd; } pt_allocated_here = true;
         uint32_t new_pde = (pt_phys & ~0xFFF) | pde_final_flags; target_pd_virt[pd_idx] = new_pde;
     } else {
         pt_phys = (uintptr_t)(pde & ~0xFFF); uint32_t needed_pde_flags = pde_final_flags;
         if ((pde & needed_pde_flags) != needed_pde_flags) { uint32_t promoted_pde = pde | (needed_pde_flags & (PAGE_RW | PAGE_USER)); target_pd_virt[pd_idx] = promoted_pde; }
     }
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         if (pt_allocated_here) { target_pd_virt[pd_idx] = 0; BUDDY_FREE((void*)pt_phys); } // Pass 1 arg
         goto cleanup_map_page_pd;
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST; uint32_t pt_idx = PTE_INDEX(aligned_vaddr); uint32_t pte = page_table_virt[pt_idx];
     if (pte & PAGE_PRESENT) { goto cleanup_map_page_pt; }
     uint32_t new_pte = (aligned_paddr & ~0xFFF) | pte_final_flags; page_table_virt[pt_idx] = new_pte; ret = 0;
 cleanup_map_page_pt: kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
 cleanup_map_page_pd: kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     if (ret == 0) { paging_invalidate_page((void*)original_vaddr); }
     return ret;
 }
 
 int paging_map_single(uint32_t *page_directory_phys, uint32_t vaddr, uint32_t paddr, uint64_t flags) { /* Definition from previous version */
     return map_page_internal(page_directory_phys, vaddr, paddr, flags | PAGE_PRESENT, false);
 }
 
 int paging_map_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, uint32_t phys_start_addr, size_t memsz, uint64_t flags) { /* Definition from previous version */
     if (!page_directory_phys || memsz == 0) { return -1; } flags |= PAGE_PRESENT;
     uintptr_t v = PAGE_ALIGN_DOWN(virt_start_addr); uintptr_t p = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t v_end; if (virt_start_addr > UINTPTR_MAX - memsz) { v_end = UINTPTR_MAX; } else { v_end = virt_start_addr + memsz; }
     v_end = PAGE_ALIGN_UP(v_end); if (v_end < v) v_end = UINTPTR_MAX;
     size_t required_phys_size = v_end - v; if (required_phys_size == 0 && memsz > 0) { required_phys_size = PAGE_ALIGN_UP(memsz); if (required_phys_size == 0) required_phys_size = PAGE_SIZE; v_end = v + required_phys_size; if (v_end < v) v_end = UINTPTR_MAX; }
     while (v < v_end) {
         bool can_use_large = g_pse_supported && (v % PAGE_SIZE_LARGE == 0) && (p % PAGE_SIZE_LARGE == 0) && ((v_end - v) >= PAGE_SIZE_LARGE);
         size_t step = can_use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
         if (map_page_internal(page_directory_phys, v, p, flags, can_use_large) != 0) { return -1; }
         if (v > UINTPTR_MAX - step) { break; } if (p > UINTPTR_MAX - step) { return -1; }
         v += step; p += step;
     } return 0;
 }
 
 static bool is_page_table_empty(uint32_t* pt_virt) { /* Definition from previous version */
     if (!pt_virt) return true;
     for (int i = 0; i < PAGES_PER_TABLE; ++i) { if (pt_virt[i] & PAGE_PRESENT) { return false; } }
     return true;
 }
 
 int paging_unmap_range(uint32_t *page_directory_phys, uint32_t virt_start_addr, size_t memsz) { /* Definition from previous version - ensure BUDDY_FREE takes 1 arg */
     if (!page_directory_phys || memsz == 0 || !g_kernel_page_directory_virt) { return -1; }
     uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr); uintptr_t v_end; if (virt_start_addr > UINTPTR_MAX - memsz) { v_end = UINTPTR_MAX; } else { v_end = virt_start_addr + memsz; } v_end = PAGE_ALIGN_UP(v_end); if (v_end <= v_start) { return -1; }
     uint32_t *target_pd_virt = NULL; uint32_t *pt_virt = NULL; int final_result = 0;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) { return -1; }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
     for (uintptr_t v_block_start = v_start; v_block_start < v_end; ) {
         uint32_t pd_idx = PDE_INDEX(v_block_start); uintptr_t v_block_end = PAGE_LARGE_ALIGN_UP(v_block_start + 1); if (v_block_end < v_block_start) v_block_end = UINTPTR_MAX;
         uint32_t pde = target_pd_virt[pd_idx];
         if (!(pde & PAGE_PRESENT)) { v_block_start = v_block_end; continue; }
         if (pde & PAGE_SIZE_4MB) {
             uintptr_t large_page_v_addr = PAGE_LARGE_ALIGN_DOWN(v_block_start);
             if (large_page_v_addr >= v_start && (large_page_v_addr + PAGE_SIZE_LARGE) <= v_end) {
                 uintptr_t frame_base_phys = pde & 0xFFC00000; for (int i = 0; i < PAGES_PER_TABLE; ++i) { put_frame(frame_base_phys + i * PAGE_SIZE); }
                 target_pd_virt[pd_idx] = 0; tlb_flush_range((void*)large_page_v_addr, PAGE_SIZE_LARGE);
             } else { final_result = -1; } v_block_start = large_page_v_addr + PAGE_SIZE_LARGE; continue;
         }
         uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF); pt_virt = NULL; bool pt_was_freed = false;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys_val, PTE_KERNEL_DATA_FLAGS) == 0) {
             pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST; uintptr_t loop_v_end = (v_end < v_block_end) ? v_end : v_block_end;
             for (uintptr_t v_current = v_block_start; v_current < loop_v_end; v_current += PAGE_SIZE) {
                  uint32_t pt_idx = PTE_INDEX(v_current); uint32_t pte = pt_virt[pt_idx];
                  if (pte & PAGE_PRESENT) { uintptr_t frame_phys = pte & ~0xFFF; put_frame(frame_phys); pt_virt[pt_idx] = 0; paging_invalidate_page((void*)v_current); }
             }
             if (is_page_table_empty(pt_virt)) {
                 target_pd_virt[pd_idx] = 0; paging_invalidate_page((void*)PAGE_LARGE_ALIGN_DOWN(v_block_start));
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); pt_virt = NULL; pt_was_freed = true;
                 BUDDY_FREE((void*)pt_phys_val); // Pass 1 arg
             }
             if (pt_virt && !pt_was_freed) { kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); pt_virt = NULL; }
         } else { final_result = -1; } v_block_start = v_block_end;
     } kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     return final_result;
 }
 
 int paging_identity_map_range(uint32_t *page_directory_phys, uint32_t start_addr, size_t size, uint64_t flags) { /* Definition from previous version */
     return paging_map_range(page_directory_phys, start_addr, start_addr, size, flags | PAGE_PRESENT);
 }
 
 int paging_get_physical_address(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t *paddr) { /* Definition from previous version */
     if (!page_directory_phys || !paddr || !g_kernel_page_directory_virt) { return -1; } int ret = -1;
     uint32_t *target_pd_virt = NULL; uint32_t *pt_virt = NULL;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) { return -1; }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_SRC; uint32_t pd_idx = PDE_INDEX(vaddr); uint32_t pde = target_pd_virt[pd_idx];
     if (pde & PAGE_PRESENT) {
         if (pde & PAGE_SIZE_4MB) { uintptr_t page_base_phys = pde & 0xFFC00000; uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1); *paddr = page_base_phys + page_offset; ret = 0; }
         else { uintptr_t pt_phys = pde & ~0xFFF;
             if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, pt_phys, PTE_KERNEL_DATA_FLAGS) == 0) {
                 pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_SRC; uint32_t pt_idx = PTE_INDEX(vaddr); uint32_t pte = pt_virt[pt_idx];
                 if (pte & PAGE_PRESENT) { uintptr_t page_base_phys = pte & ~0xFFF; uintptr_t page_offset = vaddr & (PAGE_SIZE - 1); *paddr = page_base_phys + page_offset; ret = 0; }
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC);
             }
         }
     } kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC);
     return ret;
 }
 
 // --- Page Fault Handler ---
 void page_fault_handler(registers_t *regs) { /* Definition from previous version */
     uint32_t fault_addr; asm volatile("mov %%cr2, %0" : "=r"(fault_addr)); uint32_t error_code = regs->err_code;
     bool present = (error_code & 0x1); bool write = (error_code & 0x2); bool user = (error_code & 0x4);
     bool reserved_bit = (error_code & 0x8); bool instruction_fetch = (error_code & 0x10); bool protection_key = (error_code & 0x20);
     pcb_t* current_process = get_current_process(); uint32_t current_pid = current_process ? current_process->pid : (uint32_t)-1;
     terminal_printf("\n--- PAGE FAULT (PID %u) ---\n Addr: 0x%x Code: 0x%x (%s %s %s %s %s %s)\n EIP: 0x%x CS: 0x%x EFLAGS: 0x%x\n", current_pid, fault_addr, error_code,
                     present ? "P" : "NP", write ? "W" : "R", user ? "U" : "S", reserved_bit ? "RSV" : "-", instruction_fetch ? "IF" : "DF", protection_key ? "PK/NX" : "-");
     if (user) { terminal_printf(" UserESP: 0x%x UserSS: 0x%x\n", regs->user_esp, regs->user_ss); }
     if (!user) { goto unhandled_fault_kernel; }
     if (!current_process || !current_process->mm) { goto unhandled_fault_kernel; }
     mm_struct_t *mm = current_process->mm;
     if (reserved_bit) { goto unhandled_fault_user; } if (protection_key && g_nx_supported && instruction_fetch) { goto unhandled_fault_user; }
     vma_struct_t *vma = find_vma(mm, fault_addr);
     if (!vma) { terminal_printf(" Cause: No VMA for 0x%x. Seg Fault.\n", fault_addr); goto unhandled_fault_user; }
     terminal_printf(" Fault within VMA [0x%x-0x%x) Flags=0x%x\n", vma->vm_start, vma->vm_end, vma->vm_flags);
     if (write && !(vma->vm_flags & VM_WRITE)) { terminal_printf(" Cause: Write to read-only VMA. Seg Fault.\n"); goto unhandled_fault_user; }
     if (!write && !(vma->vm_flags & VM_READ)) { terminal_printf(" Cause: Read from no-read VMA. Seg Fault.\n"); goto unhandled_fault_user; }
     if (instruction_fetch && !(vma->vm_flags & VM_EXEC)) { terminal_printf(" Cause: IF from non-exec VMA. Seg Fault.\n"); goto unhandled_fault_user; }
     int result = handle_vma_fault(mm, vma, fault_addr, error_code);
     if (result == 0) { terminal_printf("--- Page Fault Handled ---\n"); return; }
     else { terminal_printf(" Cause: handle_vma_fault failed (%d). Seg Fault.\n", result); goto unhandled_fault_user; }
 unhandled_fault_user: terminal_write("--- Unhandled User Page Fault ---\nTerminating process.\n--------------------------\n"); remove_current_task_with_code(0xDEAD000F); PAGING_PANIC("remove_current_task");
 unhandled_fault_kernel: terminal_write("--- Unhandled Kernel Page Fault ---\n"); PAGING_PANIC("Irrecoverable Kernel Page Fault");
 }
 
 
 // --- Process Management Related ---
 void paging_free_user_space(uint32_t *page_directory_phys) { /* Definition from previous version - ensure BUDDY_FREE takes 1 arg */
     if (!page_directory_phys || !g_kernel_page_directory_virt) { return; }
     uint32_t *target_pd_virt = NULL;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) { return; }
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
     for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t pde = target_pd_virt[i];
         if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) { uintptr_t pt_phys = pde & ~0xFFF; BUDDY_FREE((void*)pt_phys); } // Pass 1 arg
         target_pd_virt[i] = 0;
     } kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
 }
 
 uintptr_t paging_clone_directory(uint32_t *src_page_directory_phys) { /* Definition from previous version - ensure BUDDY_FREE takes 1 arg */
     if (!src_page_directory_phys || !g_kernel_page_directory_virt) { return 0; }
     uintptr_t dst_pd_phys = paging_alloc_frame_buddy(); if (!dst_pd_phys) { return 0; }
     uint32_t *src_pd_virt = NULL; uint32_t *dst_pd_virt = NULL; uint32_t *src_pt_virt = NULL; uint32_t *dst_pt_virt = NULL; int error_occurred = 0;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)src_page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone; } src_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, dst_pd_phys, PTE_KERNEL_DATA_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone; } dst_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
     for (size_t i = KERNEL_PDE_INDEX; i < TABLES_PER_DIR; ++i) { dst_pd_virt[i] = src_pd_virt[i]; }
     for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) {
         uint32_t src_pde = src_pd_virt[i]; if (!(src_pde & PAGE_PRESENT)) { dst_pd_virt[i] = 0; continue; }
         if (src_pde & PAGE_SIZE_4MB) { dst_pd_virt[i] = src_pde; continue; } // Share 4MB pages for now
         uintptr_t src_pt_phys = src_pde & ~0xFFF; uintptr_t dst_pt_phys = (uintptr_t)allocate_page_table_phys();
         if (!dst_pt_phys) { error_occurred = 1; goto cleanup_clone; }
         src_pt_virt = NULL; dst_pt_virt = NULL;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, src_pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) { BUDDY_FREE((void*)dst_pt_phys); error_occurred = 1; goto cleanup_clone; } src_pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, dst_pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) { kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC); BUDDY_FREE((void*)dst_pt_phys); error_occurred = 1; goto cleanup_clone; } dst_pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
         for (int j = 0; j < PAGES_PER_TABLE; ++j) { uint32_t src_pte = src_pt_virt[j]; if (src_pte & PAGE_PRESENT) { dst_pt_virt[j] = src_pte; /* TODO: CoW */ } else { dst_pt_virt[j] = 0; } }
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC); src_pt_virt = NULL; kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); dst_pt_virt = NULL;
         uint32_t dst_pde = (dst_pt_phys & ~0xFFF) | (src_pde & 0xFFF); dst_pd_virt[i] = dst_pde;
     }
 cleanup_clone:
     if (src_pd_virt) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC); if (dst_pd_virt) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     if (error_occurred) { paging_free_user_space((uint32_t*)dst_pd_phys); BUDDY_FREE((void*)dst_pd_phys); return 0; } // Pass 1 arg
     return dst_pd_phys;
 }