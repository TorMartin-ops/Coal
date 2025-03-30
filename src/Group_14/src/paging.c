#include "paging.h"
#include "terminal.h"   // For debug output (optional)
#include "buddy.h"      // Buddy allocator used to allocate new page tables

#include "libc/stdint.h"
#include "libc/stddef.h"

// Global pointer to the currently active page directory.
static uint32_t* kernel_page_directory = 0;

/**
 * paging_set_directory - Sets the global page directory pointer.
 *
 * @param pd Pointer to the new page directory.
 */
void paging_set_directory(uint32_t* pd) {
    kernel_page_directory = pd;
}

/**
 * allocate_page_table - Dynamically allocates a new page table using the buddy allocator.
 *
 * @return Pointer to a new, zeroed page table (aligned to 4096 bytes), or NULL on failure.
 */
static uint32_t* allocate_page_table(void) {
    uint32_t* pt = (uint32_t*)buddy_alloc(4096);
    if (!pt) {
        terminal_write("paging: Failed to allocate page table.\n");
        return 0;
    }
    // Zero out the new page table.
    for (int i = 0; i < 1024; i++) {
        pt[i] = 0;
    }
    return pt;
}

/**
 * paging_map_page - Maps a single virtual address to an identity mapping.
 *
 * This function uses the global kernel_page_directory. If the page table for the
 * corresponding directory entry does not exist, it is allocated dynamically.
 *
 * @param virt_addr The virtual address to map.
 */
void paging_map_page(void* virt_addr) {
    if (!kernel_page_directory) {
        terminal_write("Error: kernel_page_directory not set in paging_map_page!\n");
        return;
    }
    uint32_t addr = (uint32_t)virt_addr;
    uint32_t directory_index = addr >> 22;
    uint32_t table_index = (addr >> 12) & 0x3FF;

    uint32_t pd_entry = kernel_page_directory[directory_index];
    uint32_t* page_table;

    if (!(pd_entry & PAGE_PRESENT)) {
        page_table = allocate_page_table();
        if (!page_table)
            return;
        kernel_page_directory[directory_index] = ((uint32_t)page_table) | PAGE_PRESENT | PAGE_RW;
    } else {
        page_table = (uint32_t*)(pd_entry & ~0xFFF);
    }

    // Create identity mapping.
    page_table[table_index] = addr | PAGE_PRESENT | PAGE_RW;
    __asm__ volatile ("invlpg (%0)" : : "r" (virt_addr) : "memory");
}

/**
 * paging_map_range - Maps a contiguous virtual address range to identity mappings.
 *
 * @param page_directory The page directory to update.
 * @param virt_addr      Starting virtual address.
 * @param memsz          Size of the region in bytes.
 * @param flags          Flags to apply to each page (e.g., PAGE_PRESENT | PAGE_RW).
 * @return 0 on success, non-zero on failure.
 */
int paging_map_range(uint32_t *page_directory, uint32_t virt_addr, uint32_t memsz, uint32_t flags) {
    if (!page_directory) {
        terminal_write("paging_map_range: Page directory is NULL.\n");
        return -1;
    }
    // Round memsz up to the nearest page boundary.
    uint32_t start_addr = virt_addr & ~(4095);
    uint32_t end_addr = (virt_addr + memsz + 4095) & ~(4095);

    for (uint32_t addr = start_addr; addr < end_addr; addr += 4096) {
        // Compute directory and table indices.
        uint32_t directory_index = addr >> 22;
        uint32_t table_index = (addr >> 12) & 0x3FF;
        uint32_t pd_entry = page_directory[directory_index];
        uint32_t* page_table;
        if (!(pd_entry & PAGE_PRESENT)) {
            page_table = allocate_page_table();
            if (!page_table) {
                terminal_write("paging_map_range: Failed to allocate page table.\n");
                return -1;
            }
            page_directory[directory_index] = ((uint32_t)page_table) | (flags & 0x7);
            // Ensure RW flag is set for our allocations.
            page_directory[directory_index] |= PAGE_RW;
        } else {
            page_table = (uint32_t*)(pd_entry & ~0xFFF);
        }
        page_table[table_index] = addr | flags | PAGE_RW;
        __asm__ volatile ("invlpg (%0)" : : "r" (addr) : "memory");
    }
    return 0;
}
