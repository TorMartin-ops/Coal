#ifndef PAGING_H
 #define PAGING_H

 #include <kernel/core/types.h>
 #include <kernel/sync/spinlock.h>
 // Include kmalloc_internal for ALIGN_UP needed by PAGE_ALIGN_UP
 #include <kernel/memory/kmalloc_internal.h> // Ensure this path is correct

 #ifdef __cplusplus
 extern "C" {
 #endif

 // --- Page Size Definitions ---
 #ifndef PAGE_SIZE
 #define PAGE_SIZE 4096u
 #endif
 #ifndef PAGE_SIZE_LARGE
 #define PAGE_SIZE_LARGE (4u * 1024u * 1024u) // 4MiB
 #endif
 #define PAGES_PER_TABLE 1024u
 #define TABLES_PER_DIR  1024u

 // --- Page Table/Directory Entry Flags ---
 #define PAGE_PRESENT    0x001   // Page is present in memory
 #define PAGE_RW         0x002   // Read/Write permission
 #define PAGE_USER       0x004   // User/Supervisor level access (0=Sup, 1=User)
 #define PAGE_PWT        0x008   // Page Write-Through caching
 #define PAGE_PCD        0x010   // Page Cache Disable
 #define PAGE_ACCESSED   0x020   // Accessed bit (set by CPU)
 #define PAGE_DIRTY      0x040   // Dirty bit (set by CPU on write, only in PTE)
 #define PAGE_SIZE_4MB   0x080   // Page Size Extension bit (in PDE for 4MB pages)
 #define PAGE_GLOBAL     0x100   // Global bit (prevents TLB flush on CR3 change if PGE enabled)
 // Bits 9-11 (0xE00) are available for OS use
 #define PAGE_OS_AVAILABLE_1 0x200 // Example: Use Bit 9 for OS specific flag (e.g., NX tracking)
 #define PAGE_OS_AVAILABLE_2 0x400 // Example: Use Bit 10
 #define PAGE_OS_AVAILABLE_3 0x800 // Example: Use Bit 11

 // --- No-Execute (NX) Bit Tracking ---
 // In 32-bit non-PAE mode, NX is enabled via EFER.NXE and implicitly applies.
 // We use an available software bit (e.g., bit 9) to track intent if needed.
 #define PAGE_NX_BIT     PAGE_OS_AVAILABLE_1 // Use Bit 9 (0x200) to signify NX intention in software

 // --- Common Flag Combinations (32-bit focus) ---
 // Kernel flags should *never* have PAGE_USER set.
 // RWX Permissions:
 // R = PAGE_PRESENT must be set.
 // W = PAGE_RW must be set.
 // X = Depends on EFER.NXE. If NXE=1, page is X only if PAGE_NX_BIT is *not* set. If NXE=0, all readable pages are X.
 #define PTE_KERNEL_DATA_FLAGS     (PAGE_PRESENT | PAGE_RW | PAGE_NX_BIT)     // Kernel RW-, NX
 // ---> FIX #4: Removed PAGE_RW from KERNEL_CODE_FLAGS <---
 #define PTE_KERNEL_CODE_FLAGS     (PAGE_PRESENT)                             // Kernel R-X (NX bit implicitly clear or ignored for kernel)
 // ---> End Fix #4 <---
 #define PTE_KERNEL_READONLY_FLAGS (PAGE_PRESENT | PAGE_NX_BIT)               // Kernel R--, NX
 #define PTE_USER_DATA_FLAGS (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX_BIT) // User RW-, NX
 #define PTE_USER_CODE_FLAGS (PAGE_PRESENT | PAGE_USER)                     // User R-X (No PAGE_RW, No PAGE_NX_BIT)

 #define PDE_FLAGS_FROM_PTE(pte_flags) ((pte_flags) & (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD))

 // --- Page Table Entry Address Masks (32-bit non-PAE) ---
 #define PAGING_FLAG_MASK          0xFFF      // Lower 12 bits are flags/available
 #define PAGING_ADDR_MASK          0xFFFFF000 // Upper 20 bits are physical frame number (PFN)
 #define PAGING_PTE_ADDR_MASK      PAGING_ADDR_MASK // Mask for PTE address portion
 #define PAGING_PDE_ADDR_MASK_4KB  PAGING_ADDR_MASK // Mask for PDE pointing to 4KB PT address portion
 #define PAGING_PDE_ADDR_MASK_4MB  0xFFC00000 // Upper 10 bits are PFN for 4MB page (bits 22-31)

 // Mask helpers
 #define PAGING_PAGE_MASK   (~(PAGE_SIZE - 1u)) // Align down mask (e.g., 0xFFFFF000)
 #define PAGING_OFFSET_MASK (PAGE_SIZE - 1u)   // Offset mask (e.g., 0x00000FFF)


 // --- Virtual Memory Layout ---
 #ifndef KERNEL_SPACE_VIRT_START
 #define KERNEL_SPACE_VIRT_START 0xC0000000u // Match actual kernel mapping address
 #endif

 // --- Helper Macros ---
 // Calculate PDE/PTE index from virtual address
 #define PAGING_PDE_SHIFT 22
 #define PAGING_PTE_SHIFT 12
 #define PDE_INDEX(addr)  (((uintptr_t)(addr) >> PAGING_PDE_SHIFT) & 0x3FFu) // Index: bits 22-31
 #define PTE_INDEX(addr)  (((uintptr_t)(addr) >> PAGING_PTE_SHIFT) & 0x3FFu) // Index: bits 12-21
 #define PAGE_OFFSET(addr) ((uintptr_t)(addr) & (PAGE_SIZE - 1))           // Offset: bits 0-11

 // Calculate index based on KERNEL_SPACE_VIRT_START
 #define KERNEL_PDE_INDEX PDE_INDEX(KERNEL_SPACE_VIRT_START) // Index of the PDE covering the kernel base


 // Align address down/up to page boundaries
 #ifndef PAGE_ALIGN_DOWN // Guard against potential redefinition
 #define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1u))
 #endif
 #ifndef PAGE_ALIGN_UP   // Requires ALIGN_UP from kmalloc_internal.h
 #define PAGE_ALIGN_UP(addr)   ALIGN_UP(addr, PAGE_SIZE)
 #endif

 // Align address down/up to large page boundaries
 #define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1u))
 #define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1u) & ~(PAGE_SIZE_LARGE - 1u))

 // --- Recursive Mapping ---
 // Maps the last PDE to point back to the Page Directory base address
 #define RECURSIVE_PDE_INDEX 1023u // Index of the last PDE (0x3FF)
 // Base virtual address for accessing Page Tables via recursive mapping:
 #define RECURSIVE_PDE_VADDR 0xFFC00000u
 // Base virtual address for accessing the Page Directory itself via recursive mapping:
 #define RECURSIVE_PD_VADDR  0xFFFFF000u // Points to the PD itself (last 4KB of address space)


 // --- Physical Address Constants ---
 #ifndef VGA_PHYS_ADDR
 #define VGA_PHYS_ADDR 0xB8000u
 #endif
 // Define VGA virtual address (typically in higher half)
 #ifndef VGA_VIRT_ADDR
 #define VGA_VIRT_ADDR (KERNEL_SPACE_VIRT_START + VGA_PHYS_ADDR)
 #endif


 // --- Temporary Kernel Mapping Area ---
 // Define a RANGE of virtual addresses for dynamic temporary mappings
 #define KERNEL_TEMP_MAP_START 0xFE000000u  // Start of the temporary mapping VA range (16MB)
 #define KERNEL_TEMP_MAP_END   0xFF000000u  // End of the temporary mapping VA range
 #define KERNEL_TEMP_MAP_SIZE  (KERNEL_TEMP_MAP_END - KERNEL_TEMP_MAP_START)
 #define KERNEL_TEMP_MAP_COUNT (KERNEL_TEMP_MAP_SIZE / PAGE_SIZE) // Number of temp slots (4096)

 #define KERNEL_STACK_VADDR_START 0xE0000000

 // --- CPU Features / Control Register Bits / MSRs ---
 // CR4 Bits
 #define CR4_PSE (1 << 4) // Page Size Extension (Enable 4MB pages)
 #define CR4_PAE (1 << 5) // Physical Address Extension (Enable >4GB RAM + NX bit in PTEs) - NOT USED HERE
 #define CR4_PGE (1 << 7) // Page Global Enable (Works with PAGE_GLOBAL flag)

 // CPUID Feature Bits (EDX from basic features, Leaf 1)
 #define CPUID_FEAT_EDX_PSE (1 << 3)  // Processor supports Page Size Extension
 #define CPUID_FEAT_EDX_PAE (1 << 6)  // Processor supports PAE

 // CPUID Feature Bits (EDX from extended features, Leaf 0x80000001)
 #define CPUID_FEAT_EDX_NX (1 << 20) // Processor supports NX bit (Execute Disable)

 // MSR Addresses
 #define MSR_EFER 0xC0000080 // Extended Feature Enable Register

 // EFER Bits (Note: EFER is 64-bit MSR)
 #define EFER_NXE (1ULL << 11) // No-Execute Enable

#define PAGE_FAULT_PRESENT  (1 << 0) // P bit: 0=Not Present, 1=Protection Violation
#define PAGE_FAULT_WRITE    (1 << 1) // W/R bit: 0=Read, 1=Write
#define PAGE_FAULT_USER     (1 << 2) // U/S bit: 0=Supervisor, 1=User


 typedef struct registers {
    // Pushed by common ISR/IRQ stubs (before pusha)
    uint32_t gs, fs, es, ds;
    // Pushed by PUSHA instruction
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    // Pushed by assembly stub AFTER CPU state
    uint32_t int_no;
    // Pushed by CPU for hardware exceptions (or 0 by stub if none)
    uint32_t err_code;
    // Pushed by CPU automatically on interrupt/exception
    uint32_t eip, cs, eflags;
    uint32_t useresp; // User stack pointer
    uint32_t ss;      // User stack segment
} __attribute__((packed)) registers_t; // <<< ADDED PACKED ATTRIBUTE HERE


 // --- Global Paging Variables (Defined in paging.c) ---
 extern bool g_pse_supported;               // True if CPU supports 4MB pages (PSE)
 extern bool g_nx_supported;                // True if CPU supports No-Execute (via EFER)
 extern uint32_t* g_kernel_page_directory_virt; // Virtual address of the kernel's page directory
 extern uint32_t g_kernel_page_directory_phys; // Physical address of the kernel's page directory


 // --- Public Paging Function Prototypes ---

 // Initialization and Activation
 bool check_and_enable_pse(void);
 void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys);
 int paging_initialize_directory(uintptr_t* out_pd_phys);
 int paging_setup_early_maps(uintptr_t page_directory_phys,
                             uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
                             uintptr_t heap_phys_start, size_t heap_size);
 int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes);
 void paging_activate(uint32_t *page_directory_phys); // Implemented in ASM

 // Mapping and Unmapping
 int paging_map_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, uintptr_t phys_start_addr, size_t memsz, uint32_t flags);
 int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz);
 int paging_map_single_4k(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags);

 // Utilities and Process Management
 void page_fault_handler(registers_t *regs);
 void paging_free_user_space(uint32_t *page_directory_phys);
 uintptr_t paging_clone_directory(uint32_t* src_pd_phys);
 int paging_get_physical_address(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t *paddr);
 void copy_kernel_pde_entries(uint32_t *new_pd_virt);

 // TLB Management
 void paging_invalidate_page(void *vaddr); // Implemented in ASM
 void tlb_flush_range(void* start, size_t size);

 // --- NEW Dynamic Temporary Mapping ---
 /**
  * @brief Initializes the dynamic temporary virtual address allocator.
  * Must be called after paging is active and kmalloc is initialized.
  */
 int paging_temp_map_init(void);

 /**
  * @brief Temporarily maps a physical page into a dynamically allocated kernel
  * virtual address from the KERNEL_TEMP_MAP range.
  *
  * @param phys_addr The physical address of the page frame to map (must be page-aligned).
  * @param flags     The desired page table entry flags (e.g., PTE_KERNEL_DATA_FLAGS).
  * @return A kernel virtual address where the page is mapped, or NULL on failure
  * (e.g., out of temporary virtual addresses, mapping failed).
  */
 void* paging_temp_map(uintptr_t phys_addr, uint32_t flags);

 /**
  * @brief Unmaps a previously allocated temporary virtual address.
  *
  * @param temp_vaddr The virtual address returned by paging_temp_map.
  */
 void paging_temp_unmap(void* temp_vaddr);

 int paging_get_physical_address_and_flags(uint32_t *page_directory_phys,
    uintptr_t vaddr,
    uintptr_t *paddr_out,
    uint32_t *flags_out);


 #ifdef __cplusplus
 }
 #endif

 #endif // PAGING_H