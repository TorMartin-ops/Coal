#ifndef PAGING_H
#define PAGING_H

#include "types.h"
#include "spinlock.h"
// Include kmalloc_internal for ALIGN_UP needed by PAGE_ALIGN_UP
#include "kmalloc_internal.h" // Ensure this path is correct

#ifdef __cplusplus
extern "C" {
#endif

// --- Page Size Definitions ---
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef PAGE_SIZE_LARGE
#define PAGE_SIZE_LARGE (4 * 1024 * 1024) // 4MiB
#endif
#define PAGES_PER_TABLE 1024
#define TABLES_PER_DIR  1024

// --- Page Table/Directory Entry Flags ---
#define PAGE_PRESENT    0x001  // Page is present in memory
#define PAGE_RW         0x002  // Read/Write permission
#define PAGE_USER       0x004  // User/Supervisor level access
#define PAGE_PWT        0x008  // Page Write-Through caching
#define PAGE_PCD        0x010  // Page Cache Disable
#define PAGE_ACCESSED   0x020  // Accessed bit (set by CPU)
#define PAGE_DIRTY      0x040  // Dirty bit (set by CPU on write, only in PTE)
#define PAGE_SIZE_4MB   0x080  // Page Size Extension bit (in PDE for 4MB pages)
#define PAGE_GLOBAL     0x100  // Global bit (prevents TLB flush on CR3 change if PGE enabled)
// Bits 9-11 (0x600) are available for OS use
// Note: PAGE_NX (0x8000000000000000) relevant for 64-bit/PAE; 32-bit uses EFER.NXE

// --- Common Flag Combinations (32-bit focus) ---
// NX bit is controlled by EFER.NXE register, not flags in 32-bit non-PAE mode.
#define PTE_KERNEL_DATA_FLAGS     (PAGE_PRESENT | PAGE_RW)         // Kernel RW-NX (Implicit NX)
#define PTE_KERNEL_CODE_FLAGS     (PAGE_PRESENT | PAGE_RW)         // Kernel RWX (Implicit NX does *not* block kernel code)
#define PTE_KERNEL_READONLY_FLAGS (PAGE_PRESENT)                   // Kernel R--NX (Implicit NX)
#define PTE_USER_DATA_FLAGS       (PAGE_PRESENT | PAGE_RW | PAGE_USER) // User RW-NX (Implicit NX)
#define PTE_USER_CODE_FLAGS       (PAGE_PRESENT | PAGE_RW | PAGE_USER) // User RWX
#define PDE_FLAGS_FROM_PTE(pte_flags) ((pte_flags) & (PAGE_PRESENT | PAGE_RW | PAGE_USER)) // PDE permissions needed for PT


// --- Virtual Memory Layout ---
#ifndef KERNEL_SPACE_VIRT_START
#define KERNEL_SPACE_VIRT_START 0xC0000000 // Default higher half start address
#endif

// --- Helper Macros ---
// Calculate PDE/PTE index from virtual address
#define PDE_INDEX(addr)  (((uintptr_t)(addr) >> 22) & 0x3FF)
#define PTE_INDEX(addr)  (((uintptr_t)(addr) >> 12) & 0x3FF)
#define PAGE_OFFSET(addr) ((uintptr_t)(addr) & 0xFFF)

// Calculate index based on KERNEL_SPACE_VIRT_START (ensure it's defined first)
#define KERNEL_PDE_INDEX PDE_INDEX(KERNEL_SPACE_VIRT_START) // Index of the PDE covering the kernel base


// Align address down/up to page boundaries
#ifndef PAGE_ALIGN_DOWN // Guard against potential redefinition
#define PAGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))
#endif
#ifndef PAGE_ALIGN_UP   // Requires ALIGN_UP from kmalloc_internal.h
#define PAGE_ALIGN_UP(addr)   ALIGN_UP(addr, PAGE_SIZE)
#endif

// Align address down/up to large page boundaries
#define PAGE_LARGE_ALIGN_DOWN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE_LARGE - 1))
#define PAGE_LARGE_ALIGN_UP(addr)   (((uintptr_t)(addr) + PAGE_SIZE_LARGE - 1) & ~(PAGE_SIZE_LARGE - 1))

// --- Recursive Mapping ---
#define RECURSIVE_PDE_INDEX 1023
#define RECURSIVE_PDE_VADDR 0xFFC00000u // Base virtual address for recursive PD access

// --- Physical Address Constants ---
#ifndef VGA_PHYS_ADDR
#define VGA_PHYS_ADDR 0xB8000
#endif

// --- Temporary Kernel Mapping Addresses ---
// Ensure these are unique and reserved in the kernel's virtual address space
#ifndef TEMP_MAP_ADDR_PD_SRC
#define TEMP_MAP_ADDR_PD_SRC (KERNEL_SPACE_VIRT_START - 1 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PT_SRC
#define TEMP_MAP_ADDR_PT_SRC (KERNEL_SPACE_VIRT_START - 2 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PD_DST
#define TEMP_MAP_ADDR_PD_DST (KERNEL_SPACE_VIRT_START - 3 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PT_DST
#define TEMP_MAP_ADDR_PT_DST (KERNEL_SPACE_VIRT_START - 4 * PAGE_SIZE)
#endif
#ifndef TEMP_MAP_ADDR_PF
#define TEMP_MAP_ADDR_PF     (KERNEL_SPACE_VIRT_START - 5 * PAGE_SIZE) // Example
#endif


// --- CPU State Structure (Used by Page Fault Handler) ---
// This structure defines the layout of registers pushed by the ISR stubs.
typedef struct registers {
    // Pushed by 'pushad' instruction (in reverse order)
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    // Pushed by assembly ISR stub (segment registers)
    uint32_t ds, es, fs, gs;
     // Pushed by assembly ISR stub (interrupt number) and CPU (error code)
    uint32_t int_no, err_code;
    // Pushed by the CPU automatically on interrupt/exception
    uint32_t eip, cs, eflags;
    // Pushed by CPU only if a privilege change occurred (e.g., user to kernel)
    uint32_t user_esp, user_ss;
} registers_t;


// Structure for defining memory regions to map during early initialization
typedef struct {
    const char* name;           // Descriptive name for logging/errors
    uintptr_t phys_start;       // Physical start address
    uintptr_t phys_end;         // Physical end address (exclusive)
    uint32_t flags;             // PTE flags (use 32-bit flags for this target)
    bool map_higher_half;       // Map identity (false) or higher-half (true)
    bool required;              // If true, panic if mapping fails or size is zero
} early_memory_region_t;

// --- Global Paging Variables (Defined in paging.c) ---
extern bool g_pse_supported;                  // True if CPU supports 4MB pages (PSE)
extern bool g_nx_supported;                   // True if CPU supports No-Execute (via EFER)
extern uint32_t* g_kernel_page_directory_virt; // Virtual address of the kernel's page directory (after paging is enabled)
extern uint32_t g_kernel_page_directory_phys; // Physical address of the kernel's page directory


// --- Public Paging Function Prototypes ---

/**
 * @brief Checks if the CPU supports Page Size Extension (PSE) and enables it in CR4.
 * @return true if PSE is supported and enabled, false otherwise.
 */
bool check_and_enable_pse(void);

/**
 * @brief Sets the global pointers for the kernel's page directory.
 * Called after the PD is mapped into the higher half via recursive mapping.
 * @param pd_virt Virtual address of the kernel page directory (e.g., 0xFFC00000).
 * @param pd_phys Physical address of the kernel page directory.
 */
void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys);

/**
 * @brief Allocates and prepares the initial kernel page directory structure.
 * Checks CPU features (PSE, NX). Does NOT activate paging.
 * @param out_pd_phys Pointer to store the physical address of the allocated PD frame.
 * @return 0 on success, negative error code on failure.
 */
int paging_initialize_directory(uintptr_t* out_pd_phys);

/**
 * @brief Sets up essential early mappings needed before paging is activated.
 * Maps kernel sections (.text, .rodata, .data) into the higher half and the
 * buddy heap region identity mapped. Avoids kernel identity mapping.
 * Uses the early frame allocator for needed page tables.
 * @param page_directory_phys Physical address of the page directory being built.
 * @param kernel_phys_start Physical start address of the kernel (unused, uses linker symbols).
 * @param kernel_phys_end Physical end address of the kernel (unused, uses linker symbols).
 * @param heap_phys_start Physical start address of the early heap (buddy).
 * @param heap_size Size of the early heap.
 * @return 0 on success, panics on failure.
 */
int paging_setup_early_maps(uintptr_t page_directory_phys,
                            uintptr_t kernel_phys_start, uintptr_t kernel_phys_end,
                            uintptr_t heap_phys_start, size_t heap_size);


/**
 * @brief Finalizes kernel mappings (sets recursive entry) and activates paging.
 * Requires the Buddy allocator to be initialized for potential temporary mappings.
 * Sets global PD pointers. Maps physical memory to higher half.
 * @param page_directory_phys Physical address of the fully prepared page directory.
 * @param total_memory_bytes Total physical memory detected.
 * @return 0 on success, panics on failure.
 */
int paging_finalize_and_activate(uintptr_t page_directory_phys, uintptr_t total_memory_bytes);

/**
 * @brief Maps a single 4KB virtual page to a physical frame.
 * Allocates page tables if necessary using the primary frame allocator.
 * @param page_directory_phys Physical address of the target page directory.
 * @param vaddr Virtual address to map.
 * @param paddr Physical address of the frame to map to.
 * @param flags Page Table Entry flags (PAGE_PRESENT, PAGE_RW, PAGE_USER).
 * @return 0 on success, negative error code on failure.
 */
int paging_map_single_4k(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t paddr, uint32_t flags);


/**
 * @brief Maps a range of virtual addresses to a contiguous physical memory range.
 * Attempts to use 4MB large pages (if supported and aligned) where possible.
 * @param page_directory_phys Physical address of the target page directory.
 * @param virt_start_addr Start virtual address (will be page-aligned down).
 * @param phys_start_addr Start physical address (will be page-aligned down).
 * @param memsz Size of the memory range to map.
 * @param flags Page Table Entry flags to apply (use 32-bit flags).
 * @return 0 on success, negative error code on failure.
 */
int paging_map_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, uintptr_t phys_start_addr, size_t memsz, uint32_t flags);

/**
 * @brief Unmaps a range of virtual addresses.
 * Frees associated physical frames using the Frame Allocator (`put_frame`).
 * Frees page table frames using the Frame Allocator if they become empty.
 * @param page_directory_phys Physical address of the target page directory.
 * @param virt_start_addr Start virtual address of the range to unmap.
 * @param memsz Size of the range to unmap.
 * @return 0 on success, negative error code on failure.
 */
int paging_unmap_range(uint32_t *page_directory_phys, uintptr_t virt_start_addr, size_t memsz);

/**
 * @brief Invalidates a single page entry in the TLB (Translation Lookaside Buffer).
 * @param vaddr The virtual address whose TLB entry should be flushed.
 */
void paging_invalidate_page(void *vaddr);

/**
 * @brief Flushes TLB entries for a range of virtual addresses.
 * @param start Start virtual address of the range.
 * @param size Size of the range.
 */
void tlb_flush_range(void* start, size_t size);

/**
 * @brief Loads the specified page directory physical address into CR3 to activate it.
 * Also enables the PG bit in CR0 to turn paging on. (Used internally).
 * @param page_directory_phys Physical address of the page directory to activate.
 */
void paging_activate(uint32_t *page_directory_phys);

/**
 * @brief The C-level handler called by the assembly stub for Page Faults (Interrupt 14).
 * Analyzes the fault address and error code, potentially involving the VMA system (mm.c).
 * @param regs Pointer to the saved register state.
 */
void page_fault_handler(registers_t *regs);

/**
 * @brief Frees all user-space page tables associated with a page directory.
 * Iterates through the lower part (user space) of the PD, freeing PT frames using the Frame Allocator.
 * Does NOT free the Page Directory frame itself or unmap data pages referenced by the PTs.
 * @param page_directory_phys Physical address of the page directory to clean up.
 */
void paging_free_user_space(uint32_t *page_directory_phys);


/**
 * @brief Creates a new page directory by cloning an existing one.
 * Copies user-space PDEs/PTEs (shares physical frames initially, increments ref counts)
 * and shares kernel PDEs. Requires paging and recursive mapping to be active.
 * @param src_page_directory_phys Physical address of the source PD.
 * @return Physical address of the new PD on success, 0 on failure.
 */
uintptr_t paging_clone_directory(uint32_t* src_pd_phys);

/**
 * @brief Gets the physical address corresponding to a virtual address in a given PD.
 * Uses temporary kernel mappings to inspect the PD/PTs.
 * @param page_directory_phys Physical address of the target page directory.
 * @param vaddr Virtual address to translate.
 * @param paddr Output pointer to store the resulting physical address.
 * @return 0 on success (physical address stored in paddr), negative if mapping not found or invalid.
 */
int paging_get_physical_address(uint32_t *page_directory_phys, uintptr_t vaddr, uintptr_t *paddr);

// Note: Early mapping/allocation functions are typically static to paging.c
// uintptr_t paging_alloc_early_frame_physical(void);
// int paging_map_physical_early(uintptr_t page_directory_phys, uintptr_t phys_addr_start, size_t size, uint32_t flags, bool map_to_higher_half);


#ifdef __cplusplus
}
#endif

#endif // PAGING_H