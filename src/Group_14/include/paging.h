#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Page Size Definition ---
#define PAGE_SIZE 4096

// --- Page Table/Directory Entry Flags ---
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
// Add other flags as needed (Accessed, Dirty, PCD, PWT, Global, etc.)

// --- Virtual Memory Layout Constants ---
#define KERNEL_SPACE_VIRT_START 0xC0000000 // Virtual address where kernel space begins

/**
 * @brief Sets the global kernel_page_directory pointer.
 * Declared in paging.c, used by other modules.
 */
extern uint32_t* kernel_page_directory;

/**
 * @brief Sets the global kernel_page_directory pointer.
 *
 * Stores the pointer to the currently active kernel page directory.
 * Does NOT load it into CR3.
 *
 * @param pd Physical address pointer to the page directory.
 */
void paging_set_directory(uint32_t* pd);

/**
 * @brief Maps a single virtual page to its identical physical address.
 *
 * Ensures the page table exists (allocating if necessary) and sets the PTE.
 * Uses the globally stored kernel_page_directory.
 *
 * @param virt_addr The virtual (and physical) address to map.
 */
void paging_map_page(void* virt_addr);

/**
 * @brief Maps a range of virtual addresses to their identical physical addresses.
 *
 * Allocates page tables if needed. Rounds the range to page boundaries.
 *
 * @param page_directory The page directory to modify (physical address).
 * @param virt_addr Start virtual address of the range.
 * @param memsz Size of the range in bytes.
 * @param flags Flags to apply to the page table entries (e.g., PAGE_PRESENT | PAGE_RW).
 * @return 0 on success, -1 on failure (e.g., out of memory).
 */
int paging_map_range(uint32_t *page_directory, uint32_t virt_addr,
                     uint32_t memsz, uint32_t flags);

/**
 * @brief Helper function to create identity mapping for a range [0..size).
 *
 * Maps physical addresses 0 to 'size' to the same virtual addresses.
 * Useful during initial kernel setup.
 *
 * @param page_directory The page directory to modify (physical address).
 * @param size The upper bound (exclusive) of physical memory to map (will be page-aligned up).
 * @param flags Flags to apply (e.g., PAGE_PRESENT | PAGE_RW).
 * @return 0 on success, -1 on failure.
 */
int paging_init_identity_map(uint32_t *page_directory, uint32_t size, uint32_t flags); // Added declaration

/**
 * @brief Activates a given page directory.
 *
 * Loads the physical address of the page directory into CR3 and ensures
 * the paging enable bit (PG) in CR0 is set.
 *
 * @param page_directory Physical address pointer to the page directory to activate.
 */
void paging_activate(uint32_t *page_directory); // Added declaration


#ifdef __cplusplus
}
#endif

#endif // PAGING_H