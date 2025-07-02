/**
 * @file paging_process.c
 * @brief Process-specific paging operations implementation
 */

#define LOG_MODULE "paging_proc"

#include <kernel/memory/paging_process.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_internal.h>
#include <kernel/memory/paging_temp.h>
#include <kernel/memory/frame.h>
#include <kernel/interfaces/logger.h>
#include <kernel/lib/string.h>

// Page Size bit for 4MB pages in PDEs
#define PAGE_PS 0x80  // Bit 7 - Page Size (0=4KB page table, 1=4MB page)

// External globals
extern uint32_t* g_kernel_page_directory_virt;
extern uint32_t g_kernel_page_directory_phys;

/**
 * @brief Copy kernel page directory entries to a new page directory
 * 
 * Copies all kernel space PDEs (typically indices 768-1022) from the
 * kernel's page directory to the new process page directory.
 */
void copy_kernel_pde_entries(uint32_t* new_pd_virt) {
    if (!new_pd_virt || !g_kernel_page_directory_virt) {
        LOGGER_ERROR(LOG_MODULE, "Invalid page directory pointers");
        return;
    }
    
    // Copy kernel PDEs (everything at or above KERNEL_SPACE_VIRT_START)
    // This typically starts at PDE index 768 (0xC0000000 / 4MB)
    uint32_t kernel_pde_start = PDE_INDEX(KERNEL_SPACE_VIRT_START);
    
    LOGGER_DEBUG(LOG_MODULE, "Copying kernel PDEs from index %u to 1022", kernel_pde_start);
    
    for (uint32_t i = kernel_pde_start; i < 1023; i++) {
        new_pd_virt[i] = g_kernel_page_directory_virt[i];
    }
    
    // Note: Don't copy the recursive mapping (index 1023) - that's set separately
}

/**
 * @brief Switch to a different page directory
 */
void paging_switch_directory(uint32_t pd_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

/**
 * @brief Get current page directory physical address
 */
uint32_t paging_get_current_directory(void) {
    uint32_t pd_phys;
    asm volatile("mov %%cr3, %0" : "=r"(pd_phys));
    return pd_phys;
}

/**
 * @brief Clone a page directory for a new process
 */
uintptr_t paging_clone_directory(uint32_t* src_pd_phys) {
    // Allocate a new page directory
    uintptr_t new_pd_phys = frame_alloc();
    if (!new_pd_phys) {
        LOGGER_ERROR(LOG_MODULE, "Failed to allocate frame for new page directory");
        return 0;
    }
    
    // Map the new page directory temporarily
    uint32_t* new_pd_virt = (uint32_t*)paging_temp_map(new_pd_phys);
    if (!new_pd_virt) {
        put_frame(new_pd_phys);
        return 0;
    }
    
    // Clear the new page directory
    memset(new_pd_virt, 0, PAGE_SIZE);
    
    // Copy kernel mappings
    copy_kernel_pde_entries(new_pd_virt);
    
    // Set up recursive mapping for the new PD
    new_pd_virt[RECURSIVE_PDE_INDEX] = new_pd_phys | PAGE_PRESENT | PAGE_RW;
    
    // TODO: Clone user space mappings if needed (for fork())
    
    paging_temp_unmap((uintptr_t)new_pd_virt);
    
    LOGGER_DEBUG(LOG_MODULE, "Cloned page directory: %#x -> %#lx", (uint32_t)src_pd_phys, new_pd_phys);
    
    return new_pd_phys;
}

/**
 * @brief Free all user space mappings in a page directory
 */
void paging_free_user_space(uint32_t* page_directory_phys) {
    // Map the page directory
    uint32_t* pd_virt = (page_directory_phys == (uint32_t*)g_kernel_page_directory_phys)
                        ? g_kernel_page_directory_virt
                        : (uint32_t*)paging_temp_map((uintptr_t)page_directory_phys);
    
    if (!pd_virt) {
        LOGGER_ERROR(LOG_MODULE, "Failed to map page directory for cleanup");
        return;
    }
    
    // Free all user space page tables (below kernel space)
    uint32_t kernel_pde_start = PDE_INDEX(KERNEL_SPACE_VIRT_START);
    
    for (uint32_t i = 0; i < kernel_pde_start; i++) {
        uint32_t pde = pd_virt[i];
        
        if (!(pde & PAGE_PRESENT)) {
            continue;
        }
        
        if (pde & PAGE_PS) {
            // 4MB page - free it
            uintptr_t large_page_phys = pde & PAGING_PDE_ADDR_MASK_4MB;
            // TODO: Handle 4MB page freeing
            LOGGER_WARN(LOG_MODULE, "4MB page freeing not implemented");
        } else {
            // Page table - free all pages in it, then free the PT itself
            uintptr_t pt_phys = pde & PAGING_ADDR_MASK;
            uint32_t* pt_virt = (uint32_t*)paging_temp_map(pt_phys);
            
            if (pt_virt) {
                // Free all pages in this page table
                for (uint32_t j = 0; j < 1024; j++) {
                    uint32_t pte = pt_virt[j];
                    if (pte & PAGE_PRESENT) {
                        uintptr_t page_phys = pte & PAGING_ADDR_MASK;
                        put_frame(page_phys);
                    }
                }
                
                paging_temp_unmap((uintptr_t)pt_virt);
            }
            
            // Free the page table itself
            put_frame(pt_phys);
        }
        
        // Clear the PDE
        pd_virt[i] = 0;
    }
    
    // Unmap if we used temp mapping
    if (pd_virt != g_kernel_page_directory_virt) {
        paging_temp_unmap((uintptr_t)pd_virt);
    }
    
    LOGGER_DEBUG(LOG_MODULE, "Freed user space for PD %p", page_directory_phys);
}