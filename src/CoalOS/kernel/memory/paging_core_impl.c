/**
 * @file paging_core_impl.c
 * @brief Core paging operations implementation
 * 
 * This file contains the actual implementation of core paging functions
 * that were missing from the refactored codebase.
 */

#define LOG_MODULE "paging"

#include <kernel/memory/paging.h>
#include <kernel/memory/paging_core.h>
#include <kernel/memory/paging_internal.h>
#include <kernel/memory/paging_temp.h>
#include <kernel/memory/frame.h>
#include <kernel/interfaces/logger.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>

// External globals
extern uint32_t* g_kernel_page_directory_virt;
extern uint32_t g_kernel_page_directory_phys;

/**
 * @brief Map a range of physical memory to virtual addresses
 */
int paging_map_range(uint32_t* page_directory_phys, 
                     uintptr_t virt_start_addr,
                     uintptr_t phys_start_addr, 
                     size_t memsz, 
                     uint32_t flags) {
    
    // Validate alignment
    if ((virt_start_addr & (PAGE_SIZE - 1)) || (phys_start_addr & (PAGE_SIZE - 1))) {
        LOGGER_ERROR(LOG_MODULE, "Addresses not page-aligned: virt=%p, phys=%p",
                     (void*)virt_start_addr, (void*)phys_start_addr);
        return -1;
    }
    
    // Calculate number of pages
    size_t num_pages = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    
    LOGGER_INFO(LOG_MODULE, "Mapping range: pd_phys=%p, virt=%p, phys=%p, pages=%zu, flags=%#x",
                 page_directory_phys, (void*)virt_start_addr, (void*)phys_start_addr, num_pages, flags);
    
    // Map each page
    for (size_t i = 0; i < num_pages; i++) {
        uintptr_t vaddr = virt_start_addr + (i * PAGE_SIZE);
        uintptr_t paddr = phys_start_addr + (i * PAGE_SIZE);
        
        int result = paging_map_single_4k(page_directory_phys, vaddr, paddr, flags);
        if (result != 0) {
            LOGGER_ERROR(LOG_MODULE, "Failed to map page at virt=%p, phys=%p, result=%d", 
                         (void*)vaddr, (void*)paddr, result);
            // TODO: Unmap already mapped pages on failure
            return result;
        }
    }
    
    return 0;
}

/**
 * @brief Unmap a range of virtual addresses
 */
int paging_unmap_range(uint32_t* page_directory_phys,
                       uintptr_t virt_start_addr,
                       size_t memsz) {
    
    // Validate alignment
    if (virt_start_addr & (PAGE_SIZE - 1)) {
        LOGGER_ERROR(LOG_MODULE, "Virtual address not page-aligned: %p", (void*)virt_start_addr);
        return -1;
    }
    
    // Calculate number of pages
    size_t num_pages = (memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    
    LOGGER_DEBUG(LOG_MODULE, "Unmapping range: virt=%p, pages=%zu",
                 (void*)virt_start_addr, num_pages);
    
    // Get virtual address of page directory
    uint32_t* pd_virt = (page_directory_phys == (uint32_t*)g_kernel_page_directory_phys) 
                        ? g_kernel_page_directory_virt 
                        : (uint32_t*)paging_temp_map((uintptr_t)page_directory_phys);
    
    if (!pd_virt) {
        LOGGER_ERROR(LOG_MODULE, "Failed to map page directory");
        return -1;
    }
    
    // Unmap each page
    for (size_t i = 0; i < num_pages; i++) {
        uintptr_t vaddr = virt_start_addr + (i * PAGE_SIZE);
        
        // Get page directory index and entry
        uint32_t pd_index = (vaddr >> 22) & 0x3FF;
        uint32_t* pde = &pd_virt[pd_index];
        
        if (!(*pde & PAGE_PRESENT)) {
            continue; // Already unmapped
        }
        
        // Get page table
        uintptr_t pt_phys = *pde & PAGING_ADDR_MASK;
        uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
        if (!pt_virt) {
            continue;
        }
        
        // Clear page table entry
        uint32_t pt_index = (vaddr >> 12) & 0x3FF;
        pt_virt[pt_index] = 0;
        
        // Invalidate TLB
        paging_invalidate_page((void*)vaddr);
        
        paging_temp_unmap((uintptr_t)pt_virt);
    }
    
    // Unmap temporary mapping if used
    if (pd_virt != g_kernel_page_directory_virt) {
        paging_temp_unmap((uintptr_t)pd_virt);
    }
    
    return 0;
}

/**
 * @brief Map a single 4KB page
 */
int paging_map_single_4k(uint32_t* page_directory_phys,
                         uintptr_t vaddr,
                         uintptr_t paddr,
                         uint32_t flags) {
    
    // Validate alignment
    if ((vaddr & (PAGE_SIZE - 1)) || (paddr & (PAGE_SIZE - 1))) {
        LOGGER_ERROR(LOG_MODULE, "Addresses not page-aligned: virt=%p, phys=%p",
                     (void*)vaddr, (void*)paddr);
        return -1;
    }
    
    // Get page directory index and page table index
    uint32_t pd_index = (vaddr >> 22) & 0x3FF;
    uint32_t pt_index = (vaddr >> 12) & 0x3FF;
    
    // Map the page directory if it's not the kernel PD
    uint32_t* pd_virt;
    bool pd_is_temp = false;
    
    if (page_directory_phys == (uint32_t*)g_kernel_page_directory_phys) {
        pd_virt = g_kernel_page_directory_virt;
    } else {
        pd_virt = (uint32_t*)paging_temp_map((uintptr_t)page_directory_phys);
        if (!pd_virt) {
            LOGGER_ERROR(LOG_MODULE, "Failed to map page directory");
            return -1;
        }
        pd_is_temp = true;
    }
    
    // Get page directory entry
    uint32_t* pde = &pd_virt[pd_index];
    bool need_new_pt = false;
    uintptr_t pt_phys = 0;
    
    // Check if page table exists
    if (!(*pde & PAGE_PRESENT)) {
        // Allocate new page table
        pt_phys = frame_alloc();
        if (!pt_phys) {
            LOGGER_ERROR(LOG_MODULE, "Failed to allocate page table");
            if (pd_is_temp) paging_temp_unmap((uintptr_t)pd_virt);
            return -1;
        }
        need_new_pt = true;
    } else {
        pt_phys = *pde & PAGING_ADDR_MASK;
    }
    
    // If we need a new page table, clear it first
    if (need_new_pt) {
        // For process PDs, we need to be careful about temp mapping exhaustion
        // Set the PDE first, then use recursive mapping if possible
        if (pd_is_temp) {
            // Set page directory entry before unmapping PD
            *pde = pt_phys | PAGE_PRESENT | PAGE_RW | (vaddr >= KERNEL_SPACE_VIRT_START ? 0 : PAGE_USER);
            
            // Unmap the PD to free up a temp slot
            paging_temp_unmap((uintptr_t)pd_virt);
            pd_is_temp = false;
            
            // Now map and clear the page table
            uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
            if (!pt_virt) {
                put_frame(pt_phys);
                return -1;
            }
            memset(pt_virt, 0, PAGE_SIZE);
            
            // Set the PTE
            pt_virt[pt_index] = paddr | flags | PAGE_PRESENT;
            
            // Unmap page table
            paging_temp_unmap((uintptr_t)pt_virt);
        } else {
            // Kernel PD - can use temp mapping normally
            uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
            if (!pt_virt) {
                put_frame(pt_phys);
                return -1;
            }
            memset(pt_virt, 0, PAGE_SIZE);
            
            // Set page directory entry
            *pde = pt_phys | PAGE_PRESENT | PAGE_RW | (vaddr >= KERNEL_SPACE_VIRT_START ? 0 : PAGE_USER);
            
            // Set the PTE
            pt_virt[pt_index] = paddr | flags | PAGE_PRESENT;
            
            // Unmap page table
            paging_temp_unmap((uintptr_t)pt_virt);
        }
    } else {
        // Page table already exists - map it and set PTE
        
        // If we still have the PD mapped temporarily, unmap it first
        if (pd_is_temp) {
            paging_temp_unmap((uintptr_t)pd_virt);
            pd_is_temp = false;
        }
        
        // Map the page table
        uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
        if (!pt_virt) {
            return -1;
        }
        
        // Set page table entry
        pt_virt[pt_index] = paddr | flags | PAGE_PRESENT;
        
        // Unmap page table
        paging_temp_unmap((uintptr_t)pt_virt);
    }
    
    // Invalidate TLB
    paging_invalidate_page((void*)vaddr);
    
    return 0;
}

/**
 * @brief Get physical address for a virtual address
 */
int paging_get_physical_address(uint32_t* page_directory_phys,
                                uintptr_t vaddr,
                                uintptr_t* paddr) {
    uint32_t flags;
    return paging_get_physical_address_and_flags(page_directory_phys, vaddr, paddr, &flags);
}

/**
 * @brief Get physical address and flags for a virtual address
 */
int paging_get_physical_address_and_flags(uint32_t* page_directory_phys,
                                          uintptr_t vaddr,
                                          uintptr_t* paddr_out,
                                          uint32_t* flags_out) {
    
    // Get virtual address of page directory
    uint32_t* pd_virt = (page_directory_phys == (uint32_t*)g_kernel_page_directory_phys) 
                        ? g_kernel_page_directory_virt 
                        : (uint32_t*)paging_temp_map((uintptr_t)page_directory_phys);
    
    if (!pd_virt) {
        LOGGER_ERROR(LOG_MODULE, "Failed to map page directory");
        return -1;
    }
    
    // Get page directory entry
    uint32_t pd_index = (vaddr >> 22) & 0x3FF;
    uint32_t pde = pd_virt[pd_index];
    
    if (!(pde & PAGE_PRESENT)) {
        if (pd_virt != g_kernel_page_directory_virt) {
            paging_temp_unmap((uintptr_t)pd_virt);
        }
        return -1; // Not mapped
    }
    
    // Get page table
    uintptr_t pt_phys = pde & PAGING_ADDR_MASK;
    uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
    if (!pt_virt) {
        if (pd_virt != g_kernel_page_directory_virt) {
            paging_temp_unmap((uintptr_t)pd_virt);
        }
        return -1;
    }
    
    // Get page table entry
    uint32_t pt_index = (vaddr >> 12) & 0x3FF;
    uint32_t pte = pt_virt[pt_index];
    
    if (!(pte & PAGE_PRESENT)) {
        paging_temp_unmap((uintptr_t)pt_virt);
        if (pd_virt != g_kernel_page_directory_virt) {
            paging_temp_unmap((uintptr_t)pd_virt);
        }
        return -1; // Not mapped
    }
    
    // Calculate physical address
    *paddr_out = (pte & PAGING_ADDR_MASK) | (vaddr & (PAGE_SIZE - 1));
    *flags_out = pte & ~PAGING_ADDR_MASK;
    
    // Clean up temporary mappings
    paging_temp_unmap((uintptr_t)pt_virt);
    if (pd_virt != g_kernel_page_directory_virt) {
        paging_temp_unmap((uintptr_t)pd_virt);
    }
    
    return 0;
}

/**
 * @brief Flush TLB entries for a range of addresses
 */
void tlb_flush_range(void* start, size_t size) {
    uintptr_t addr = (uintptr_t)start & ~(PAGE_SIZE - 1);
    uintptr_t end = ((uintptr_t)start + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    while (addr < end) {
        paging_invalidate_page((void*)addr);
        addr += PAGE_SIZE;
    }
}