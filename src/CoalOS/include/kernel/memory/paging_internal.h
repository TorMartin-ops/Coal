/**
 * @file paging_internal.h
 * @brief Internal definitions shared between paging modules
 */

#ifndef COAL_MEMORY_PAGING_INTERNAL_H
#define COAL_MEMORY_PAGING_INTERNAL_H

#include <kernel/memory/paging.h>
#include <kernel/core/error.h>
#include <kernel/sync/spinlock.h>

// Module identification for logging
#define LOG_MODULE_PAGING      "paging"
#define LOG_MODULE_PAGING_TEMP "paging_temp"
#define LOG_MODULE_PAGING_PROC "paging_proc"

// Internal constants
#define MAX_EARLY_FRAMES_TRACKED 1024

// Early boot allocation tracking
extern uintptr_t g_early_frames_allocated[MAX_EARLY_FRAMES_TRACKED];
extern size_t g_early_frames_count;
extern spinlock_t g_early_frames_lock;

// Shared internal functions
error_t validate_alignment(uintptr_t addr, size_t alignment, const char* desc);
error_t validate_address_range(uintptr_t start, size_t size);
bool is_kernel_address(uintptr_t addr);
bool is_user_address(uintptr_t addr);

// Page table entry manipulation
static inline uint32_t pte_get_flags(uint32_t pte) {
    return pte & PAGING_FLAG_MASK;
}

static inline uintptr_t pte_get_address(uint32_t pte) {
    return pte & PAGING_ADDR_MASK;
}

static inline uint32_t pte_set_address(uint32_t pte, uintptr_t addr) {
    return (addr & PAGING_ADDR_MASK) | (pte & PAGING_FLAG_MASK);
}

static inline bool pte_is_present(uint32_t pte) {
    return (pte & PAGE_PRESENT) != 0;
}

static inline bool pte_is_writable(uint32_t pte) {
    return (pte & PAGE_RW) != 0;
}

static inline bool pte_is_user(uint32_t pte) {
    return (pte & PAGE_USER) != 0;
}

#endif // COAL_MEMORY_PAGING_INTERNAL_H