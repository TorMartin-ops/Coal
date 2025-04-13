/**
 * paging.c - Paging Implementation (32-bit x86 with PSE and NX via EFER)
 *
 * Refactored Version:
 * - Addresses build errors related to `early_memory_region_t`.
 * - Uses `uint32_t` for flags in mapping functions suitable for 32-bit target.
 * - Improves structure and error handling in early mapping stages.
 * - Validates allocator results and addresses potential overflows.
 * - Uses kernel panic macro for fatal errors.
 * - Moves assembly-heavy functions (paging_invalidate_page, paging_activate)
 * to paging_asm.asm.
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
 // Definitions like PAGE_SIZE, PDE_INDEX, PTE_INDEX, PAGE flags,
 // KERNEL_SPACE_VIRT_START, TEMP_MAP addresses, VGA_PHYS_ADDR, etc., are
 // assumed to be correctly defined in paging.h or included headers.

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
 uint32_t g_kernel_page_directory_phys = 0;
 bool g_pse_supported = false;
 bool g_nx_supported = false; // Controlled via EFER.NXE

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
 #define MAX_EARLY_ALLOCATIONS 128 // Max frames allocatable before buddy
 static uintptr_t early_allocated_frames[MAX_EARLY_ALLOCATIONS];
 static int early_allocated_count = 0;
 static bool early_allocator_used = false; // Track if used

 // --- External Assembly Functions ---
 // These are now defined in paging_asm.asm
 extern void paging_invalidate_page(void *vaddr);
 extern void paging_activate(uint32_t *page_directory_phys);


 // --- Forward Declarations (Internal Functions) ---
 static bool is_page_table_empty(uint32_t* pt_virt);
 static int map_page_internal(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags, bool use_large_page);
 static inline void enable_cr4_pse(void);
 static inline uint32_t read_cr4(void);
 static inline void write_cr4(uint32_t value);
 static bool check_and_enable_nx(void);
 static uintptr_t paging_alloc_frame(bool use_early_allocator);
 static uint32_t* allocate_page_table_phys(bool use_early_allocator);
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type);
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr);
 static int paging_map_physical_early(uintptr_t page_directory_phys, uintptr_t phys_addr_start, size_t size, uint32_t flags, bool map_to_higher_half);


 // --- Low-Level CPU Control ---
 static inline uint32_t read_cr4(void) { uint32_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
 static inline void write_cr4(uint32_t v) { asm volatile("mov %0, %%cr4" :: "r"(v)); }
 static inline void enable_cr4_pse(void) { write_cr4(read_cr4() | (1 << 4)); }

 // --- Low-Level Temporary Mappings (Use with EXTREME caution) ---
 // Map a physical frame to a temporary virtual address in kernel space.
 static int kernel_map_virtual_to_physical_unsafe(uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
     if (!g_kernel_page_directory_virt) return -1;
     if (vaddr < KERNEL_SPACE_VIRT_START || (vaddr % PAGE_SIZE != 0) || (paddr % PAGE_SIZE != 0)) return -1;

     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];

     if (!(pde & PAGE_PRESENT) || (pde & PAGE_SIZE_4MB)) {
         terminal_printf("[KMapUnsafe] Error: No PT or 4MB page at V=0x%x\n", vaddr);
         return -1;
     }
     uintptr_t pt_phys = pde & ~0xFFF;
     uint32_t *pt_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + pt_phys);

     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         // terminal_printf("[KMapUnsafe] Warning: Overwriting existing PTE at V=0x%x\n", vaddr);
     }
     pt_virt[pt_idx] = (paddr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
     paging_invalidate_page((void*)vaddr); // Call external asm function
     return 0;
 }

 // Unmap a temporary virtual address.
 static void kernel_unmap_virtual_unsafe(uintptr_t vaddr) {
     if (!g_kernel_page_directory_virt) return;
     if (vaddr < KERNEL_SPACE_VIRT_START || (vaddr % PAGE_SIZE != 0)) return;

     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pt_idx = PTE_INDEX(vaddr);
     uint32_t pde = g_kernel_page_directory_virt[pd_idx];

     if (!(pde & PAGE_PRESENT) || (pde & PAGE_SIZE_4MB)) return;

     uintptr_t pt_phys = pde & ~0xFFF;
     uint32_t *pt_virt = (uint32_t*)(KERNEL_SPACE_VIRT_START + pt_phys);

     if (pt_virt[pt_idx] & PAGE_PRESENT) {
         pt_virt[pt_idx] = 0;
         paging_invalidate_page((void*)vaddr); // Call external asm function
     }
 }

 // --- Early Memory Allocation (Before Buddy is ready) ---
 static struct multiboot_tag *find_multiboot_tag_early(uint32_t mb_info_phys_addr, uint16_t type) {
     // (Implementation remains the same)
     if (mb_info_phys_addr == 0 || mb_info_phys_addr >= 0x100000) return NULL;
     uint32_t total_size = *(volatile uint32_t*)mb_info_phys_addr;
     if (total_size < 8 || total_size > 0x10000) return NULL;
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     uintptr_t info_end = mb_info_phys_addr + total_size;
     while ((uintptr_t)tag < info_end && tag->type != MULTIBOOT_TAG_TYPE_END) {
         uintptr_t current_tag_addr = (uintptr_t)tag;
         if (current_tag_addr + sizeof(struct multiboot_tag) > info_end || tag->size < 8 || current_tag_addr + tag->size > info_end) return NULL;
         if (tag->type == type) return tag;
         uintptr_t next_tag_addr = current_tag_addr + ((tag->size + 7) & ~7);
         if (next_tag_addr >= info_end) break;
         tag = (struct multiboot_tag *)next_tag_addr;
     }
     return NULL;
 }

 // Allocates a single, zeroed physical frame using the Multiboot map.
 static uintptr_t paging_alloc_early_frame_physical(void) {
     // (Implementation remains the same)
      early_allocator_used = true;
     if (g_multiboot_info_phys_addr_global == 0) PAGING_PANIC("Early alloc attempted before Multiboot info set!");
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)
         find_multiboot_tag_early(g_multiboot_info_phys_addr_global, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) PAGING_PANIC("Early alloc failed: Multiboot MMAP tag not found!");

     uintptr_t kernel_start_p = (uintptr_t)_kernel_start_phys;
     uintptr_t kernel_end_p = PAGE_ALIGN_UP((uintptr_t)_kernel_end_phys);
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

     while ((uintptr_t)mmap_entry < mmap_end) {
         if (mmap_tag->entry_size == 0 || (uintptr_t)mmap_entry + mmap_tag->entry_size > mmap_end) PAGING_PANIC("Invalid MMAP entry bounds");
         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->len >= PAGE_SIZE) {
             uintptr_t region_start = (uintptr_t)mmap_entry->addr;
             uintptr_t region_end = region_start + (uintptr_t)mmap_entry->len;
             if (region_end < region_start) region_end = UINTPTR_MAX; // Handle overflow
             uintptr_t current_frame_addr = ALIGN_UP(region_start, PAGE_SIZE);

             while (current_frame_addr < region_end && (current_frame_addr + PAGE_SIZE) > current_frame_addr) {
                 if (current_frame_addr < 0x100000) { current_frame_addr += PAGE_SIZE; continue; } // Skip below 1MB
                 bool overlaps_kernel = (current_frame_addr < kernel_end_p && (current_frame_addr + PAGE_SIZE) > kernel_start_p);
                 if (overlaps_kernel) { current_frame_addr += PAGE_SIZE; continue; }
                 uint32_t mb_info_size = *(volatile uint32_t*)g_multiboot_info_phys_addr_global;
                 uintptr_t mb_info_end = g_multiboot_info_phys_addr_global + (mb_info_size >= 8 ? mb_info_size : 8);
                 bool overlaps_mb_info = (current_frame_addr < mb_info_end && (current_frame_addr + PAGE_SIZE) > g_multiboot_info_phys_addr_global);
                 if (overlaps_mb_info) { current_frame_addr += PAGE_SIZE; continue; }
                 bool already_allocated = false;
                 for (int i = 0; i < early_allocated_count; ++i) {
                     if (early_allocated_frames[i] == current_frame_addr) { already_allocated = true; break; }
                 }
                 if (already_allocated) { current_frame_addr += PAGE_SIZE; continue; }

                 if (early_allocated_count >= MAX_EARLY_ALLOCATIONS) PAGING_PANIC("Exceeded MAX_EARLY_ALLOCATIONS!");

                 early_allocated_frames[early_allocated_count++] = current_frame_addr;
                 memset((void*)current_frame_addr, 0, PAGE_SIZE); // Zero the frame
                 return current_frame_addr;
             }
         }
         uintptr_t next_entry_addr = (uintptr_t)mmap_entry + mmap_tag->entry_size;
         if (next_entry_addr > mmap_end || next_entry_addr < (uintptr_t)mmap_entry) break;
         mmap_entry = (multiboot_memory_map_t *)next_entry_addr;
     }
     PAGING_PANIC("Early alloc failed: No suitable frame found!");
     return 0; // Unreachable
 }

 // --- Unified Frame/Page Table Allocation ---
 static uintptr_t paging_alloc_frame(bool use_early_allocator) {
     if (use_early_allocator) {
         if (early_allocator_used) {
             return paging_alloc_early_frame_physical();
         } else {
             PAGING_PANIC("Attempted early frame allocation after buddy init!");
         }
     }
     // Use the frame allocator (which uses buddy) after early stage
     uintptr_t frame = frame_alloc();
     if(frame == 0) {
         PAGING_PANIC("frame_alloc() failed!"); // Critical if frame allocator fails
     }
     return frame;
 }

 static uint32_t* allocate_page_table_phys(bool use_early_allocator) {
     uintptr_t pt_phys = paging_alloc_frame(use_early_allocator);
     if (!pt_phys) {
         terminal_printf("[Paging] Failed to allocate frame for Page Table (early=%d).\n", use_early_allocator);
         return NULL;
     }
     // Frame should be zeroed by the allocator (early or frame_alloc->buddy->temp map)
     return (uint32_t*)pt_phys;
 }

 // --- CPU Feature Detection ---
 bool check_and_enable_pse(void) {
      // (Implementation remains the same)
     uint32_t eax, ebx, ecx, edx;
     cpuid(1, &eax, &ebx, &ecx, &edx);
     if (edx & (1 << 3)) { // Check PSE bit
         terminal_write("[Paging] CPU supports PSE (4MB Pages).\n");
         enable_cr4_pse();
         if (read_cr4() & (1 << 4)) {
             terminal_write("[Paging] CR4.PSE bit enabled.\n");
             g_pse_supported = true; return true;
         } else {
             terminal_write("[Paging Error] Failed to enable CR4.PSE bit!\n");
         }
     } else {
         terminal_write("[Paging] CPU does not support PSE (4MB Pages).\n");
     }
     g_pse_supported = false; return false;
 }

 static bool check_and_enable_nx(void) {
      // (Implementation remains the same)
     uint32_t eax, ebx, ecx, edx;
     cpuid(0x80000000, &eax, &ebx, &ecx, &edx); // Check highest extended function
     if (eax < 0x80000001) {
         terminal_write("[Paging] CPUID leaf 0x80000001 not supported. Cannot check NX.\n");
         g_nx_supported = false; return false;
     }
     cpuid(0x80000001, &eax, &ebx, &ecx, &edx); // Check Feature Bits
     if (edx & (1 << 20)) { // Check XD/NX bit support
         terminal_write("[Paging] CPU supports NX (Execute Disable) bit.\n");
         uint64_t efer = rdmsr(MSR_EFER);
         efer |= (1ULL << 11); // Set NXE bit
         wrmsr(MSR_EFER, efer);
         efer = rdmsr(MSR_EFER); // Read back to verify
         if (efer & (1ULL << 11)) {
              terminal_write("[Paging] EFER.NXE bit enabled.\n");
              g_nx_supported = true; return true; // NX is supported AND enabled
         } else {
              terminal_write("[Paging Error] Failed to enable EFER.NXE bit!\n");
         }
     } else {
         terminal_write("[Paging] CPU does not support NX bit.\n");
     }
     g_nx_supported = false; return false;
 }

 // --- Paging Initialization Stages ---

 int paging_initialize_directory(uintptr_t* out_initial_pd_phys) {
      // (Implementation remains the same)
     terminal_write("[Paging Stage 1] Initializing Page Directory...\n");
     uintptr_t pd_phys = paging_alloc_early_frame_physical();
     if (!pd_phys) PAGING_PANIC("Failed to allocate initial Page Directory frame!");
     terminal_printf("  Allocated initial PD at Phys: 0x%x\n", pd_phys);
     if (!check_and_enable_pse()) PAGING_PANIC("PSE support required but not available/enabled!");
     check_and_enable_nx();
     *out_initial_pd_phys = pd_phys;
     terminal_write("[Paging Stage 1] Directory allocated, features checked/enabled.\n");
     return 0;
 }

 // Maps a range using the early allocator (must run before buddy_init)
 static int paging_map_physical_early(uintptr_t page_directory_phys, uintptr_t phys_addr_start, size_t size, uint32_t flags, bool map_to_higher_half) {
     // (Implementation remains the same, uses allocate_page_table_phys(true))
     if (page_directory_phys == 0 || size == 0) return -1;

     uintptr_t current_phys = PAGE_ALIGN_DOWN(phys_addr_start);
     uintptr_t end_phys = phys_addr_start + size;
     if (end_phys < phys_addr_start) end_phys = UINTPTR_MAX; // Handle overflow
     uintptr_t aligned_end_phys = PAGE_ALIGN_UP(end_phys);
     if (aligned_end_phys < end_phys) aligned_end_phys = UINTPTR_MAX;
     end_phys = aligned_end_phys;

     if (end_phys <= current_phys) return 0; // Not an error if size became zero

     uint32_t* pd_phys_ptr = (uint32_t*)page_directory_phys;

     while (current_phys < end_phys) {
         uintptr_t target_vaddr;
         if (map_to_higher_half) {
             if (current_phys > UINTPTR_MAX - KERNEL_SPACE_VIRT_START) return -1; // VA overflow
             target_vaddr = KERNEL_SPACE_VIRT_START + current_phys;
         } else {
             target_vaddr = current_phys;
         }
         uint32_t pd_idx = PDE_INDEX(target_vaddr);
         uint32_t pt_idx = PTE_INDEX(target_vaddr);

         uint32_t pde = pd_phys_ptr[pd_idx];
         uintptr_t pt_phys_addr;
         uint32_t* pt_phys_ptr;

         if (!(pde & PAGE_PRESENT)) {
             pt_phys_ptr = allocate_page_table_phys(true);
             if (!pt_phys_ptr) return -1;
             pt_phys_addr = (uintptr_t)pt_phys_ptr;
             uint32_t pde_flags = PDE_FLAGS_FROM_PTE(flags) | PAGE_PRESENT;
             pd_phys_ptr[pd_idx] = (pt_phys_addr & ~0xFFF) | pde_flags;
         } else {
             if (pde & PAGE_SIZE_4MB) return -1; // Conflict
             pt_phys_addr = (uintptr_t)(pde & ~0xFFF);
             pt_phys_ptr = (uint32_t*)pt_phys_addr;
             uint32_t needed_pde_flags = PDE_FLAGS_FROM_PTE(flags);
             if ((pde & needed_pde_flags) != needed_pde_flags) {
                 pd_phys_ptr[pd_idx] |= (needed_pde_flags & (PAGE_RW | PAGE_USER));
             }
         }

         uint32_t pte_flags_32 = (flags & (PAGE_RW | PAGE_USER)) | PAGE_PRESENT;
         if (pt_phys_ptr[pt_idx] & PAGE_PRESENT) return -1; // Conflict
         pt_phys_ptr[pt_idx] = (current_phys & ~0xFFF) | pte_flags_32;

         if (current_phys > UINTPTR_MAX - PAGE_SIZE) break;
         current_phys += PAGE_SIZE;
     }
     return 0;
 }

 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start __attribute__((unused)),
                             uintptr_t kernel_phys_end __attribute__((unused)),
                             uintptr_t heap_phys_start, size_t heap_size)
 {
      // (Implementation using early_memory_region_t remains the same)
     terminal_write("[Paging Stage 2] Setting up early mappings...\n");
     if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) PAGING_PANIC("Stage 2: Invalid PD physical address!");

     uintptr_t text_start   = PAGE_ALIGN_DOWN((uintptr_t)_kernel_text_start_phys);
     uintptr_t text_end     = PAGE_ALIGN_UP((uintptr_t)_kernel_text_end_phys);
     uintptr_t rodata_start = PAGE_ALIGN_DOWN((uintptr_t)_kernel_rodata_start_phys);
     uintptr_t rodata_end   = PAGE_ALIGN_UP((uintptr_t)_kernel_rodata_end_phys);
     uintptr_t data_start   = PAGE_ALIGN_DOWN((uintptr_t)_kernel_data_start_phys);
     uintptr_t data_end     = PAGE_ALIGN_UP((uintptr_t)_kernel_data_end_phys);
     uintptr_t heap_start   = PAGE_ALIGN_DOWN(heap_phys_start);
     uintptr_t heap_end     = PAGE_ALIGN_UP(heap_phys_start + heap_size); if (heap_end < heap_start) heap_end = heap_start;

     early_memory_region_t regions_to_map[] = {
         { ".text",      text_start,   text_end,   PTE_KERNEL_CODE_FLAGS,     true,  true },
         { ".rodata",    rodata_start, rodata_end, PTE_KERNEL_READONLY_FLAGS, true,  true },
         { ".data/.bss", data_start,   data_end,   PTE_KERNEL_DATA_FLAGS,     true,  true },
         { "BuddyHeap",  heap_start,   heap_end,   PTE_KERNEL_DATA_FLAGS,     false, true }, // Identity map heap
         { "VGA",        VGA_PHYS_ADDR, VGA_PHYS_ADDR + PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true, true },
         { "PD Map",     page_directory_phys, page_directory_phys + PAGE_SIZE, PTE_KERNEL_DATA_FLAGS, true, true },
     };

     for (size_t i = 0; i < ARRAY_SIZE(regions_to_map); ++i) {
         early_memory_region_t* r = &regions_to_map[i];
         size_t region_size = (r->phys_end > r->phys_start) ? (r->phys_end - r->phys_start) : 0;
         if (region_size == 0) {
             if (r->required) PAGING_PANIC("Zero-size required region");
             continue;
         }
         terminal_printf("  Mapping %-12s: Phys=[0x%x..0x%x) -> %s Flags=0x%x\n", r->name, r->phys_start, r->phys_end, r->map_higher_half ? "HigherHalf" : "Identity", r->flags);
         if (paging_map_physical_early(page_directory_phys, r->phys_start, region_size, r->flags, r->map_higher_half) != 0) {
             PAGING_PANIC("paging_map_physical_early failed");
         }
     }
     terminal_write("[Paging Stage 2] Early mappings established.\n");
     return 0;
 }

 // Finalize and activate paging (including recursive entry)
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes __attribute__((unused))) {
      // (Implementation remains the same - calls external asm paging_activate)
     terminal_write("[Paging Stage 3] Finalizing and activating paging...\n");
     if (page_directory_phys == 0 || (page_directory_phys % PAGE_SIZE) != 0) PAGING_PANIC("Finalize: Invalid PD phys addr!");

     uint32_t* pd_phys_ptr = (uint32_t*)page_directory_phys;
     uint32_t recursive_pde_flags = PAGE_PRESENT | PAGE_RW;
     if (pd_phys_ptr[RECURSIVE_PDE_INDEX] & PAGE_PRESENT) PAGING_PANIC("Recursive PDE slot already in use!");
     pd_phys_ptr[RECURSIVE_PDE_INDEX] = (page_directory_phys & ~0xFFF) | recursive_pde_flags;
     terminal_printf("  Set recursive PDE[%d] -> Phys=0x%x Flags=0x%x\n", RECURSIVE_PDE_INDEX, page_directory_phys, recursive_pde_flags);

     terminal_write("  Activating Paging (CR3, CR0.PG)...\n");
     paging_activate((uint32_t*)page_directory_phys); // Call external asm function

     uintptr_t kernel_pd_virt_addr = RECURSIVE_PDE_VADDR;
     paging_set_kernel_directory((uint32_t*)kernel_pd_virt_addr, page_directory_phys); // Call local C function

     if (!g_kernel_page_directory_virt || g_kernel_page_directory_phys != page_directory_phys) PAGING_PANIC("Failed to set global PD pointers!");
     if ((g_kernel_page_directory_virt[RECURSIVE_PDE_INDEX] & ~0xFFF) != page_directory_phys) PAGING_PANIC("Recursive entry check failed!");

     terminal_write("[Paging Stage 3] Paging enabled and active.\n");
     early_allocator_used = false;
     return 0;
 }

 // --- Post-Activation Mapping Functions ---

 // Internal helper to map a page (4k or 4M) after paging is active
 static int map_page_internal(uint32_t *target_page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags, bool use_large_page) {
      // (Implementation remains the same, uses allocate_page_table_phys(false))
      // Uses external asm paging_invalidate_page
     if (!g_kernel_page_directory_virt) PAGING_PANIC("map_page_internal called before paging fully active!");
     if (!target_page_directory_phys) return -1;

     uintptr_t aligned_vaddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(vaddr) : PAGE_ALIGN_DOWN(vaddr);
     uintptr_t aligned_paddr = use_large_page ? PAGE_LARGE_ALIGN_DOWN(paddr) : PAGE_ALIGN_DOWN(paddr);
     uint32_t pd_idx = PDE_INDEX(aligned_vaddr);
     uint32_t pde_final_flags;
     uint32_t pte_final_flags = 0;

     if (use_large_page) {
         if (!g_pse_supported) return -1;
         pde_final_flags = (flags & (PAGE_RW | PAGE_USER)) | PAGE_PRESENT | PAGE_SIZE_4MB;
     } else {
         pte_final_flags = (flags & (PAGE_RW | PAGE_USER | PAGE_GLOBAL | PAGE_DIRTY | PAGE_ACCESSED)) | PAGE_PRESENT;
         pde_final_flags = PDE_FLAGS_FROM_PTE(pte_final_flags);
     }

     uint32_t* target_pd_virt = NULL;
     uint32_t* page_table_virt = NULL;
     uintptr_t pt_phys = 0;
     int ret = -1;
     bool pt_allocated_here = false;

     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)target_page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) return -1;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;
     uint32_t pde = target_pd_virt[pd_idx];

     if (use_large_page) {
         if (pde & PAGE_PRESENT) goto cleanup_map_page;
         uint32_t new_pde = (aligned_paddr & 0xFFC00000) | pde_final_flags;
         target_pd_virt[pd_idx] = new_pde;
         ret = 0;
         goto cleanup_map_page;
     }

     if (pde & PAGE_PRESENT && (pde & PAGE_SIZE_4MB)) goto cleanup_map_page;

     if (!(pde & PAGE_PRESENT)) {
         uint32_t* pt_phys_ptr = allocate_page_table_phys(false); // Use buddy via frame_alloc
         if (!pt_phys_ptr) goto cleanup_map_page;
         pt_phys = (uintptr_t)pt_phys_ptr;
         pt_allocated_here = true;
         uint32_t new_pde = (pt_phys & ~0xFFF) | pde_final_flags | PAGE_PRESENT;
         target_pd_virt[pd_idx] = new_pde;
         paging_invalidate_page((void*)aligned_vaddr);
     } else {
         pt_phys = (uintptr_t)(pde & ~0xFFF);
         uint32_t needed_pde_flags = pde_final_flags;
         if ((pde & needed_pde_flags) != needed_pde_flags) {
             uint32_t promoted_pde = pde | (needed_pde_flags & (PAGE_RW | PAGE_USER));
             target_pd_virt[pd_idx] = promoted_pde;
             paging_invalidate_page((void*)aligned_vaddr);
         }
     }

     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) {
         if (pt_allocated_here) { target_pd_virt[pd_idx] = 0; put_frame(pt_phys); }
         goto cleanup_map_page;
     }
     page_table_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
     uint32_t pt_idx = PTE_INDEX(aligned_vaddr);
     uint32_t pte = page_table_virt[pt_idx];

     if (pte & PAGE_PRESENT) goto cleanup_map_page_pt;
     uint32_t new_pte = (aligned_paddr & ~0xFFF) | pte_final_flags;
     page_table_virt[pt_idx] = new_pte;
     ret = 0;

 cleanup_map_page_pt:
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST);
 cleanup_map_page:
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     if (ret == 0) {
         paging_invalidate_page((void*)vaddr);
     }
     return ret;
 }

 // Public function for mapping a single 4k page
 int paging_map_single_4k(uint32_t* pd_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    return map_page_internal(pd_phys, vaddr, paddr, flags, false);
 }

 // Public function for mapping a range (tries 4M pages first)
 int paging_map_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, uintptr_t phys_start_addr, size_t memsz, uint32_t flags) {
      // (Implementation remains the same)
     if (!page_directory_phys || memsz == 0) return -1;

     uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t p_start = PAGE_ALIGN_DOWN(phys_start_addr);
     uintptr_t v_end;
     if (virt_start_addr > UINTPTR_MAX - memsz) { v_end = UINTPTR_MAX; }
     else { v_end = virt_start_addr + memsz; }
     v_end = PAGE_ALIGN_UP(v_end);
     if (v_end <= v_start) { if (memsz > 0) v_end = UINTPTR_MAX; else return 0; }

     uintptr_t current_v = v_start;
     uintptr_t current_p = p_start;

     while (current_v < v_end) {
         size_t remaining_v = v_end - current_v;
         uintptr_t remaining_p_end;
         if (current_p > UINTPTR_MAX - remaining_v) { remaining_p_end = UINTPTR_MAX; }
         else { remaining_p_end = current_p + remaining_v; }
         size_t remaining_p = (remaining_p_end > current_p) ? remaining_p_end - current_p : 0;
         size_t map_size = remaining_v < remaining_p ? remaining_v : remaining_p;
         if (map_size == 0) break;

         bool use_large = g_pse_supported && (current_v % PAGE_SIZE_LARGE == 0) && (current_p % PAGE_SIZE_LARGE == 0) && (map_size >= PAGE_SIZE_LARGE);
         int result = map_page_internal(page_directory_phys, current_v, current_p, flags, use_large);
         if (result != 0) return -1;

         size_t step = use_large ? PAGE_SIZE_LARGE : PAGE_SIZE;
         if (current_v > UINTPTR_MAX - step) break;
         if (current_p > UINTPTR_MAX - step) break;
         current_v += step;
         current_p += step;
     }
     return 0;
 }

 // --- Unmapping Functions ---
 // Helper to check if a page table is empty (all PTEs non-present)
 static bool is_page_table_empty(uint32_t* pt_virt) {
      // (Implementation remains the same)
     if (!pt_virt) return true;
     for (int i = 0; i < PAGES_PER_TABLE; ++i) { if (pt_virt[i] & PAGE_PRESENT) return false; }
     return true;
 }

 // Unmaps a range, freeing physical frames and potentially page tables
 int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz) {
      // (Implementation remains the same, uses external asm paging_invalidate_page)
     if (!page_directory_phys || memsz == 0 || !g_kernel_page_directory_virt) return -1;

     uintptr_t v_start = PAGE_ALIGN_DOWN(virt_start_addr);
     uintptr_t v_end;
     if (virt_start_addr > UINTPTR_MAX - memsz) v_end = UINTPTR_MAX;
     else v_end = virt_start_addr + memsz;
     v_end = PAGE_ALIGN_UP(v_end);
     if (v_end <= v_start) return 0;

     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;
     int final_result = 0;

     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) return -1;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;

     for (uintptr_t v_block_start = v_start; v_block_start < v_end; ) {
         uint32_t pd_idx = PDE_INDEX(v_block_start);
         uintptr_t v_block_end = PAGE_LARGE_ALIGN_UP(v_block_start + 1); if (v_block_end < v_block_start) v_block_end = UINTPTR_MAX;
         uint32_t pde = target_pd_virt[pd_idx];
         if (!(pde & PAGE_PRESENT)) { v_block_start = v_block_end; continue; }

         if (pde & PAGE_SIZE_4MB) {
             uintptr_t large_page_v_addr = PAGE_LARGE_ALIGN_DOWN(v_block_start);
             if (large_page_v_addr >= v_start && (large_page_v_addr + PAGE_SIZE_LARGE) <= v_end) {
                 uintptr_t frame_base_phys = pde & 0xFFC00000;
                 for (int i = 0; i < PAGES_PER_TABLE; ++i) put_frame(frame_base_phys + i * PAGE_SIZE);
                 target_pd_virt[pd_idx] = 0;
                 tlb_flush_range((void*)large_page_v_addr, PAGE_SIZE_LARGE); // Call C function
             } else { final_result = -1; }
             v_block_start = large_page_v_addr + PAGE_SIZE_LARGE; continue;
         }

         uintptr_t pt_phys_val = (uintptr_t)(pde & ~0xFFF);
         pt_virt = NULL;
         bool pt_was_freed = false;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, pt_phys_val, PTE_KERNEL_DATA_FLAGS) == 0) {
             pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST;
             uintptr_t loop_v_end = (v_end < v_block_end) ? v_end : v_block_end;
             for (uintptr_t v_current = v_block_start; v_current < loop_v_end; v_current += PAGE_SIZE) {
                  uint32_t pt_idx = PTE_INDEX(v_current);
                  uint32_t pte = pt_virt[pt_idx];
                  if (pte & PAGE_PRESENT) {
                      uintptr_t frame_phys = pte & ~0xFFF;
                      put_frame(frame_phys);
                      pt_virt[pt_idx] = 0;
                      paging_invalidate_page((void*)v_current); // Call external asm function
                  }
             }
             if (is_page_table_empty(pt_virt)) {
                 target_pd_virt[pd_idx] = 0;
                 tlb_flush_range((void*)PAGE_LARGE_ALIGN_DOWN(v_block_start), PAGE_SIZE_LARGE); // Call C function
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); pt_virt = NULL; pt_was_freed = true;
                 put_frame(pt_phys_val);
             }
             if (pt_virt && !pt_was_freed) { kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); pt_virt = NULL; }
         } else { final_result = -1; }
         v_block_start = v_block_end;
     }
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     return final_result;
 }

 // --- Utility Functions ---
 // Gets the physical address for a virtual address (if mapped)
 int paging_get_physical_address(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t *paddr) {
      // (Implementation remains the same)
      if (!page_directory_phys || !paddr || !g_kernel_page_directory_virt) return -1;
     int ret = -1; // Not found by default
     uint32_t *target_pd_virt = NULL;
     uint32_t *pt_virt = NULL;

     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)page_directory_phys, PTE_KERNEL_READONLY_FLAGS) != 0) return -1;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
     uint32_t pd_idx = PDE_INDEX(vaddr);
     uint32_t pde = target_pd_virt[pd_idx];

     if (pde & PAGE_PRESENT) {
         if (pde & PAGE_SIZE_4MB) {
             uintptr_t page_base_phys = pde & 0xFFC00000; // Mask for 4MB base
             uintptr_t page_offset = vaddr & (PAGE_SIZE_LARGE - 1);
             *paddr = page_base_phys + page_offset;
             ret = 0; // Found
         } else { // 4KB page table
             uintptr_t pt_phys = pde & ~0xFFF;
             if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, pt_phys, PTE_KERNEL_READONLY_FLAGS) == 0) {
                 pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
                 uint32_t pt_idx = PTE_INDEX(vaddr);
                 uint32_t pte = pt_virt[pt_idx];
                 if (pte & PAGE_PRESENT) {
                     uintptr_t page_base_phys = pte & ~0xFFF;
                     uintptr_t page_offset = vaddr & (PAGE_SIZE - 1);
                     *paddr = page_base_phys + page_offset;
                     ret = 0; // Found
                 }
                 kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC); // Unmap PT
             } else { ret = -1; /* Failed to map PT */}
         }
     } // else PDE not present

     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC); // Unmap PD
     return ret;
 }

 // --- Process Management ---
 // Frees user-space page tables (but not the frames they point to)
 void paging_free_user_space(uint32_t *page_directory_phys) {
      // (Implementation remains the same)
     if (!page_directory_phys || !g_kernel_page_directory_virt) return;
     uint32_t *target_pd_virt = NULL;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, (uintptr_t)page_directory_phys, PTE_KERNEL_DATA_FLAGS) != 0) return;
     target_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;

     for (size_t i = 0; i < KERNEL_PDE_INDEX; ++i) { // Iterate only user space PDEs
         uint32_t pde = target_pd_virt[i];
         if ((pde & PAGE_PRESENT) && !(pde & PAGE_SIZE_4MB)) {
             uintptr_t pt_phys = pde & ~0xFFF;
             put_frame(pt_phys); // Free the PT frame itself
         }
         target_pd_virt[i] = 0; // Clear the PDE entry
     }
     kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
 }

 // Clones a page directory (implementing basic frame sharing, no CoW yet)
 uintptr_t paging_clone_directory(uint32_t* src_pd_phys) {
      // (Implementation remains the same)
      if (!src_pd_phys || !g_kernel_page_directory_virt) return 0;

     uintptr_t new_pd_phys = paging_alloc_frame(false); // Use frame_alloc (buddy)
     if (!new_pd_phys) return 0;

     uint32_t* src_pd_virt = NULL;
     uint32_t* dst_pd_virt = NULL;
     int error_occurred = 0;
     uintptr_t allocated_pt_phys[KERNEL_PDE_INDEX] = {0}; // Max user PTs
     int allocated_pt_count = 0;

     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_SRC, (uintptr_t)src_pd_phys, PTE_KERNEL_READONLY_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone_err; }
     src_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_SRC;
     if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PD_DST, new_pd_phys, PTE_KERNEL_DATA_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone_err; }
     dst_pd_virt = (uint32_t*)TEMP_MAP_ADDR_PD_DST;

     for (int i = KERNEL_PDE_INDEX; i < RECURSIVE_PDE_INDEX; i++) {
         dst_pd_virt[i] = g_kernel_page_directory_virt[i];
     }
     dst_pd_virt[RECURSIVE_PDE_INDEX] = (new_pd_phys & ~0xFFF) | PAGE_PRESENT | PAGE_RW;

     for (size_t i = 0; i < KERNEL_PDE_INDEX; i++) {
         uint32_t src_pde = src_pd_virt[i];
         if (!(src_pde & PAGE_PRESENT)) { dst_pd_virt[i] = 0; continue; }
         if (src_pde & PAGE_SIZE_4MB) {
             dst_pd_virt[i] = src_pde;
             uintptr_t frame_base = src_pde & 0xFFC00000;
             for(int f=0; f < PAGES_PER_TABLE; ++f) get_frame(frame_base + f*PAGE_SIZE);
             continue;
         }
         uintptr_t src_pt_phys = src_pde & ~0xFFF;
         uintptr_t dst_pt_phys = paging_alloc_frame(false);
         if (!dst_pt_phys) { error_occurred = 1; goto cleanup_clone_err; }
         allocated_pt_phys[allocated_pt_count++] = dst_pt_phys;

         uint32_t* src_pt_virt = NULL;
         uint32_t* dst_pt_virt = NULL;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_SRC, src_pt_phys, PTE_KERNEL_READONLY_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone_err; }
         src_pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_SRC;
         if (kernel_map_virtual_to_physical_unsafe(TEMP_MAP_ADDR_PT_DST, dst_pt_phys, PTE_KERNEL_DATA_FLAGS) != 0) { error_occurred = 1; goto cleanup_clone_err; }
         dst_pt_virt = (uint32_t*)TEMP_MAP_ADDR_PT_DST;

         for (int j = 0; j < PAGES_PER_TABLE; j++) {
              uint32_t src_pte = src_pt_virt[j];
              if (src_pte & PAGE_PRESENT) {
                  uintptr_t frame_phys = src_pte & ~0xFFF;
                  get_frame(frame_phys);
                  dst_pt_virt[j] = src_pte;
              } else { dst_pt_virt[j] = 0; }
         }
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_DST); dst_pt_virt = NULL;
         kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PT_SRC); src_pt_virt = NULL;
         dst_pd_virt[i] = (dst_pt_phys & ~0xFFF) | (src_pde & 0xFFF);
     }

 cleanup_clone_err:
     if (src_pd_virt) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_SRC);
     if (dst_pd_virt) kernel_unmap_virtual_unsafe(TEMP_MAP_ADDR_PD_DST);
     if (error_occurred) {
         for (int k = 0; k < allocated_pt_count; k++) { if (allocated_pt_phys[k] != 0) put_frame(allocated_pt_phys[k]); }
         if (new_pd_phys != 0) put_frame(new_pd_phys);
         return 0;
     }
     return new_pd_phys;
 }

 // --- Page Fault Handler ---
 // Definition remains the same, calls external handle_vma_fault
 void page_fault_handler(registers_t *regs) {
     // (Implementation from previous response - seems okay)
     uint32_t fault_addr; asm volatile("mov %%cr2, %0" : "=r"(fault_addr)); uint32_t error_code = regs->err_code;
     bool present = (error_code & 0x1); bool write = (error_code & 0x2); bool user = (error_code & 0x4);
     bool reserved_bit = (error_code & 0x8); bool instruction_fetch = (error_code & 0x10);

     pcb_t* current_process = get_current_process();
     uint32_t current_pid = current_process ? current_process->pid : (uint32_t)-1;

     terminal_printf("\n--- PAGE FAULT (PID %u) ---\n Addr: 0x%x Code: 0x%x (%s %s %s %s %s)\n EIP: 0x%x CS: 0x%x EFLAGS: 0x%x\n",
                     current_pid, fault_addr, error_code,
                     present ? "P" : "NP", write ? "W" : "R", user ? "U" : "S",
                     reserved_bit ? "RSV" : "-", instruction_fetch ? (g_nx_supported ? "NX" : "IF") : "DF");

     if (!user) { PAGING_PANIC("Irrecoverable Kernel Page Fault"); }

     if (!current_process || !current_process->mm) goto unhandled_fault_user;
     mm_struct_t *mm = current_process->mm;
     if (reserved_bit) goto unhandled_fault_user;
     if (g_nx_supported && instruction_fetch) {
          uintptr_t phys_addr = 0;
          if(paging_get_physical_address(mm->pgd_phys, fault_addr, &phys_addr) == 0) goto unhandled_fault_user;
     }

     vma_struct_t *vma = find_vma(mm, fault_addr);
     if (!vma) { terminal_printf(" Reason: No VMA covers 0x%x.\n", fault_addr); goto unhandled_fault_user; }
     if (write && !(vma->vm_flags & VM_WRITE)) { terminal_printf(" Reason: Write to read-only VMA.\n"); goto unhandled_fault_user; }
     if (!write && !(vma->vm_flags & VM_READ) && !instruction_fetch) { terminal_printf(" Reason: Read from no-read VMA.\n"); goto unhandled_fault_user; }
     if (instruction_fetch && !(vma->vm_flags & VM_EXEC)) { terminal_printf(" Reason: IF from non-exec VMA.\n"); goto unhandled_fault_user; }

     int result = handle_vma_fault(mm, vma, fault_addr, error_code);
     if (result == 0) return; // Handled
     else { terminal_printf(" Reason: handle_vma_fault failed (%d).\n", result); goto unhandled_fault_user; }

 unhandled_fault_user:
     terminal_printf("--- Unhandled User Page Fault ---\n UserESP: 0x%x UserSS: 0x%x\n Terminating PID %u.\n--------------------------\n", regs->user_esp, regs->user_ss, current_pid);
     remove_current_task_with_code(0xDEAD000E);
     PAGING_PANIC("remove_current_task returned!");
 }


 // Add implementations for tlb_flush_range and paging_set_kernel_directory
 // if they were missing (moved from inline assembly section)

 /**
  * @brief Flushes TLB entries for a range of virtual addresses by invalidating each page.
  * @param start Start virtual address of the range.
  * @param size Size of the range.
  */
 void tlb_flush_range(void* start, size_t size) {
     uintptr_t addr = PAGE_ALIGN_DOWN((uintptr_t)start);
     uintptr_t end_addr;

     // Calculate end address carefully to avoid overflow
     if ((uintptr_t)start > UINTPTR_MAX - size) {
         end_addr = UINTPTR_MAX;
     } else {
         end_addr = (uintptr_t)start + size;
     }
     end_addr = PAGE_ALIGN_UP(end_addr);
     if (end_addr < addr) end_addr = UINTPTR_MAX; // Handle wrap-around

     // Invalidate page by page
     while (addr < end_addr) {
         paging_invalidate_page((void*)addr); // Call external asm function
         // Check for overflow before adding PAGE_SIZE
         if (addr > UINTPTR_MAX - PAGE_SIZE) {
             break; // Stop if next step would overflow
         }
         addr += PAGE_SIZE;
     }
 }

 /**
  * @brief Sets the global pointers for the kernel's page directory.
  * Called after the PD is mapped into the higher half via recursive mapping.
  * @param pd_virt Virtual address of the kernel page directory (e.g., 0xFFC00000).
  * @param pd_phys Physical address of the kernel page directory.
  */
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
     g_kernel_page_directory_virt = pd_virt;
     g_kernel_page_directory_phys = pd_phys;
     // terminal_printf("  [Paging] Global PD set: Virt=0x%x, Phys=0x%x\n", (uintptr_t)pd_virt, pd_phys);
 }