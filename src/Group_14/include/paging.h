#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Page Size Definition ---
// Ensure this matches PAGE_SIZE in types.h or elsewhere if defined globally
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// --- Page Table/Directory Entry Flags ---
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
// Add other flags as needed (Accessed, Dirty, PCD, PWT, Global, etc.)

// --- Virtual Memory Layout Constants ---
// Ensure this matches definitions used in linker script / boot code
#ifndef KERNEL_SPACE_VIRT_START
#define KERNEL_SPACE_VIRT_START 0xC0000000 // Virtual address where kernel space begins
#endif

/**
 * @brief Global pointer to the kernel's page directory (virtual address).
 * Declared in paging.c, used by other modules.
 */
extern uint32_t* kernel_page_directory;

/**
 * @brief Sets the global kernel_page_directory pointer.
 * @param pd_virt Virtual address of the page directory.
 */
void paging_set_directory(uint32_t* pd_virt);

/**
 * @brief Maps a single virtual page to its identical physical address. (DEPRECATED)
 * Prefer using paging_map_range for clarity and flexibility.
 * @param virt_addr The virtual (and physical) address to map.
 */
void paging_map_page(void* virt_addr);

/**
 * @brief Maps a range of virtual addresses to physical addresses.
 *
 * Allocates page tables if needed. Rounds the range to page boundaries.
 *
 * @param page_directory_virt Virtual address of the page directory to modify.
 * @param virt_start_addr Start virtual address of the range.
 * @param phys_start_addr Start physical address the virtual range should map to.
 * @param memsz Size of the range in bytes.
 * @param flags Flags to apply to the page table entries (e.g., PAGE_PRESENT | PAGE_RW | PAGE_USER).
 * @return 0 on success, -1 on failure (e.g., out of memory).
 */
int paging_map_range(uint32_t *page_directory_virt, uint32_t virt_start_addr,
                     uint32_t phys_start_addr, uint32_t memsz, uint32_t flags); // Corrected return type

/**
 * @brief Helper function to create identity mapping for a range [0..size).
 *
 * Maps physical addresses 0 to 'size' to the same virtual addresses.
 * Useful during initial kernel setup. Calls paging_map_range internally.
 *
 * @param page_directory_virt Virtual address of the page directory to modify.
 * @param size The upper bound (exclusive) of physical memory to map (will be page-aligned up).
 * @param flags Flags to apply (e.g., PAGE_PRESENT | PAGE_RW).
 * @return 0 on success, -1 on failure.
 */
int paging_init_identity_map(uint32_t *page_directory_virt, uint32_t size, uint32_t flags);

/**
 * @brief Activates a given page directory and enables paging.
 *
 * Loads the physical address of the page directory into CR3 and ensures
 * the paging enable bit (PG) in CR0 is set.
 *
 * @param page_directory_phys Physical address pointer to the page directory to activate.
 */
void paging_activate(uint32_t *page_directory_phys);


#ifdef __cplusplus
}
#endif

#endif // PAGING_H