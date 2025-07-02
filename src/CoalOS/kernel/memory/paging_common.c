/**
 * @file paging_common.c
 * @brief Common utilities shared between paging modules
 */

#define LOG_MODULE "paging"

#include <kernel/memory/paging_internal.h>
#include <kernel/core/log.h>
#include <kernel/core/error.h>
#include <kernel/memory/kmalloc.h>

// Early boot allocation tracking
uintptr_t g_early_frames_allocated[MAX_EARLY_FRAMES_TRACKED];
size_t g_early_frames_count = 0;
spinlock_t g_early_frames_lock = {0};

// Global paging state
bool g_pse_supported = false;
bool g_nx_supported = false;
uint32_t* g_kernel_page_directory_virt = NULL;
uint32_t g_kernel_page_directory_phys = 0;

/**
 * @brief Validate address alignment
 */
error_t validate_alignment(uintptr_t addr, size_t alignment, const char* desc) {
    if (addr & (alignment - 1)) {
        LOG_ERROR("%s address %p not aligned to %zu bytes", desc, (void*)addr, alignment);
        return E_ALIGN;
    }
    return E_SUCCESS;
}

/**
 * @brief Validate address range doesn't wrap around
 */
error_t validate_address_range(uintptr_t start, size_t size) {
    if (size == 0) {
        LOG_WARN("Zero size memory range");
        return E_INVAL;
    }
    
    uintptr_t end = start + size;
    if (end < start) {
        LOG_ERROR("Address range overflow: start=%p, size=%zu", (void*)start, size);
        return E_OVERFLOW;
    }
    
    return E_SUCCESS;
}

/**
 * @brief Check if address is in kernel space
 */
bool is_kernel_address(uintptr_t addr) {
    return addr >= KERNEL_SPACE_VIRT_START;
}

/**
 * @brief Check if address is in user space
 */
bool is_user_address(uintptr_t addr) {
    return addr < KERNEL_SPACE_VIRT_START;
}

/**
 * @brief Set the kernel page directory
 */
void paging_set_kernel_directory(uint32_t* pd_virt, uint32_t pd_phys) {
    g_kernel_page_directory_virt = pd_virt;
    g_kernel_page_directory_phys = pd_phys;
    LOG_DEBUG("Kernel page directory set: virt=%p, phys=%#x", pd_virt, pd_phys);
}