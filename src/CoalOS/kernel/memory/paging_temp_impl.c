/**
 * @file paging_temp_impl.c
 * @brief Temporary virtual address mapping implementation
 * 
 * Provides temporary mappings for accessing physical pages that aren't
 * permanently mapped in the virtual address space.
 */

#define LOG_MODULE "paging_temp"

#include <kernel/memory/paging_temp.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_internal.h>
#include <kernel/interfaces/logger.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <kernel/lib/string.h>

// Temporary mapping configuration
// Changed from 0xFFC00000 to avoid conflict with recursive mapping at PDE 1023
#define TEMP_MAP_VIRT_START 0xFE000000  // Use 0xFE000000-0xFE010000 (PDE 1016)
#define TEMP_MAP_SLOTS      16           // Number of temp mapping slots
#define TEMP_MAP_SIZE       (TEMP_MAP_SLOTS * PAGE_SIZE)

// Temp mapping slot structure
typedef struct {
    bool in_use;
    uintptr_t phys_addr;
} temp_slot_t;

// Global state
static temp_slot_t g_temp_slots[TEMP_MAP_SLOTS];
static spinlock_t g_temp_lock = {0};
static bool g_temp_initialized = false;

// Forward declaration
void* paging_temp_map_with_flags(uintptr_t phys_addr, uint32_t flags);

/**
 * @brief Initialize the temporary mapping system
 */
int paging_temp_map_init(void) {
    if (g_temp_initialized) {
        LOGGER_WARN(LOG_MODULE, "Temporary mapping already initialized");
        return 0;
    }
    
    // Initialize slot tracking
    memset(g_temp_slots, 0, sizeof(g_temp_slots));
    spinlock_init(&g_temp_lock);
    
    // Note: The actual page table entries for the temp region should be
    // set up during early paging initialization
    
    g_temp_initialized = true;
    LOGGER_INFO(LOG_MODULE, "Temporary mapping initialized with %d slots at %p",
                TEMP_MAP_SLOTS, (void*)TEMP_MAP_VIRT_START);
    
    return 0;
}

/**
 * @brief Temporarily map a physical page to a virtual address (simplified version)
 */
void* paging_temp_map(uintptr_t phys_addr) {
    return paging_temp_map_with_flags(phys_addr, PAGE_RW);
}

/**
 * @brief Temporarily map a physical page to a virtual address with flags
 */
void* paging_temp_map_with_flags(uintptr_t phys_addr, uint32_t flags) {
    if (!g_temp_initialized) {
        LOGGER_ERROR(LOG_MODULE, "Temporary mapping not initialized");
        return NULL;
    }
    
    // Validate alignment
    if (phys_addr & (PAGE_SIZE - 1)) {
        LOGGER_ERROR(LOG_MODULE, "Physical address %p not page-aligned", (void*)phys_addr);
        return NULL;
    }
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_temp_lock);
    
    // Find a free slot
    int slot = -1;
    for (int i = 0; i < TEMP_MAP_SLOTS; i++) {
        if (!g_temp_slots[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        spinlock_release_irqrestore(&g_temp_lock, irq_flags);
        LOGGER_ERROR(LOG_MODULE, "No free temporary mapping slots");
        return NULL;
    }
    
    // Calculate virtual address for this slot
    uintptr_t vaddr = TEMP_MAP_VIRT_START + (slot * PAGE_SIZE);
    
    // Map the page
    extern uint32_t* g_kernel_page_directory_virt;
    uint32_t pd_index = (vaddr >> 22) & 0x3FF;
    uint32_t pt_index = (vaddr >> 12) & 0x3FF;
    
    // Get page table (should already exist for temp region)
    uint32_t* pde = &g_kernel_page_directory_virt[pd_index];
    if (!(*pde & PAGE_PRESENT)) {
        spinlock_release_irqrestore(&g_temp_lock, irq_flags);
        LOGGER_ERROR(LOG_MODULE, "Page table not present for temp mapping region");
        return NULL;
    }
    
    // Access the page table through recursive mapping
    // The virtual address for a PTE using recursive mapping is:
    // 0xFFC00000 + (pd_index * 0x1000) + (pt_index * 4)
    uint32_t* pte = (uint32_t*)(0xFFC00000 + (pd_index << 12) + (pt_index << 2));
    
    // Set the mapping
    *pte = phys_addr | flags | PAGE_PRESENT;
    
    // Invalidate TLB for this address
    asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
    
    // Mark slot as in use
    g_temp_slots[slot].in_use = true;
    g_temp_slots[slot].phys_addr = phys_addr;
    
    spinlock_release_irqrestore(&g_temp_lock, irq_flags);
    
    LOGGER_DEBUG(LOG_MODULE, "Mapped phys %p to temp virt %p (slot %d)",
                 (void*)phys_addr, (void*)vaddr, slot);
    
    return (void*)vaddr;
}

/**
 * @brief Unmap a temporary virtual address
 */
void paging_temp_unmap(uintptr_t temp_vaddr) {
    if (!g_temp_initialized) {
        return;
    }
    
    uintptr_t vaddr = temp_vaddr;
    
    // Validate address is in temp region
    if (vaddr < TEMP_MAP_VIRT_START || vaddr >= (TEMP_MAP_VIRT_START + TEMP_MAP_SIZE)) {
        LOGGER_ERROR(LOG_MODULE, "Address %p not in temporary mapping region", (void*)temp_vaddr);
        return;
    }
    
    // Calculate slot index
    int slot = (vaddr - TEMP_MAP_VIRT_START) / PAGE_SIZE;
    if (slot < 0 || slot >= TEMP_MAP_SLOTS) {
        LOGGER_ERROR(LOG_MODULE, "Invalid temp mapping slot");
        return;
    }
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_temp_lock);
    
    if (!g_temp_slots[slot].in_use) {
        spinlock_release_irqrestore(&g_temp_lock, irq_flags);
        LOGGER_WARN(LOG_MODULE, "Attempting to unmap unused slot %d", slot);
        return;
    }
    
    // Clear the page table entry
    extern uint32_t* g_kernel_page_directory_virt;
    uint32_t pd_index = (vaddr >> 22) & 0x3FF;
    uint32_t pt_index = (vaddr >> 12) & 0x3FF;
    
    uint32_t* pde = &g_kernel_page_directory_virt[pd_index];
    if (*pde & PAGE_PRESENT) {
        // Access the page table through recursive mapping
        uint32_t* pte = (uint32_t*)(0xFFC00000 + (pd_index << 12) + (pt_index << 2));
        *pte = 0;
        
        // Invalidate TLB
        asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
    }
    
    // Mark slot as free
    g_temp_slots[slot].in_use = false;
    g_temp_slots[slot].phys_addr = 0;
    
    spinlock_release_irqrestore(&g_temp_lock, irq_flags);
    
    LOGGER_DEBUG(LOG_MODULE, "Unmapped temp virt %p (slot %d)", (void*)temp_vaddr, slot);
}

/**
 * @brief Get statistics about temporary mappings
 */
void paging_temp_get_stats(size_t* total_slots, size_t* used_slots) {
    if (total_slots) {
        *total_slots = TEMP_MAP_SLOTS;
    }
    
    if (used_slots) {
        size_t count = 0;
        uintptr_t flags = spinlock_acquire_irqsave(&g_temp_lock);
        
        for (int i = 0; i < TEMP_MAP_SLOTS; i++) {
            if (g_temp_slots[i].in_use) {
                count++;
            }
        }
        
        spinlock_release_irqrestore(&g_temp_lock, flags);
        *used_slots = count;
    }
}

