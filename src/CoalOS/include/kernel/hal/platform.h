/**
 * @file platform.h
 * @brief Hardware Abstraction Layer (HAL) platform interface
 * 
 * This interface provides hardware abstraction following SOLID principles,
 * allowing Coal OS to be portable across different architectures.
 */

#ifndef KERNEL_HAL_PLATFORM_H
#define KERNEL_HAL_PLATFORM_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Platform capabilities flags
 */
typedef enum {
    PLATFORM_CAP_SMP        = (1 << 0),  // Symmetric multiprocessing
    PLATFORM_CAP_IOMMU      = (1 << 1),  // I/O Memory Management Unit
    PLATFORM_CAP_VIRTUALIZATION = (1 << 2), // Hardware virtualization
    PLATFORM_CAP_NX_BIT     = (1 << 3),  // No-execute bit support
    PLATFORM_CAP_PAE        = (1 << 4),  // Physical Address Extension
    PLATFORM_CAP_ACPI       = (1 << 5),  // ACPI support
} platform_capabilities_t;

/**
 * @brief Platform information structure
 */
typedef struct platform_info {
    const char *name;
    const char *vendor;
    const char *version;
    uint32_t capabilities;
    uint32_t cpu_count;
    uint64_t total_memory;
    uint32_t page_size;
} platform_info_t;

/**
 * @brief Platform operations interface
 */
typedef struct platform_operations {
    /**
     * @brief Initialize platform
     */
    error_t (*init)(void);
    
    /**
     * @brief Cleanup platform
     */
    void (*cleanup)(void);
    
    /**
     * @brief Get platform information
     */
    void (*get_info)(platform_info_t *info);
    
    /**
     * @brief Enable/disable interrupts
     */
    void (*interrupts_enable)(void);
    void (*interrupts_disable)(void);
    bool (*interrupts_enabled)(void);
    
    /**
     * @brief CPU halt/idle operations
     */
    void (*cpu_halt)(void);
    void (*cpu_idle)(void);
    
    /**
     * @brief Memory management operations
     */
    void (*invalidate_tlb)(void);
    void (*invalidate_page)(uintptr_t vaddr);
    void (*flush_cache)(void);
    
    /**
     * @brief Timer operations
     */
    uint64_t (*get_ticks)(void);
    void (*delay_microseconds)(uint32_t usec);
    
    /**
     * @brief Platform name for identification
     */
    const char *name;
} platform_operations_t;

/**
 * @brief Global platform operations (dependency injection point)
 */
extern platform_operations_t *g_platform_ops;

/**
 * @brief Set platform operations
 */
void platform_set_operations(platform_operations_t *ops);

/**
 * @brief Convenience functions using injected platform operations
 */
static inline error_t platform_init(void) {
    if (g_platform_ops && g_platform_ops->init) {
        return g_platform_ops->init();
    }
    return E_NOTSUP;
}

static inline void platform_cleanup(void) {
    if (g_platform_ops && g_platform_ops->cleanup) {
        g_platform_ops->cleanup();
    }
}

static inline void platform_get_info(platform_info_t *info) {
    if (g_platform_ops && g_platform_ops->get_info) {
        g_platform_ops->get_info(info);
    }
}

static inline void platform_interrupts_enable(void) {
    if (g_platform_ops && g_platform_ops->interrupts_enable) {
        g_platform_ops->interrupts_enable();
    }
}

static inline void platform_interrupts_disable(void) {
    if (g_platform_ops && g_platform_ops->interrupts_disable) {
        g_platform_ops->interrupts_disable();
    }
}

static inline bool platform_interrupts_enabled(void) {
    if (g_platform_ops && g_platform_ops->interrupts_enabled) {
        return g_platform_ops->interrupts_enabled();
    }
    return false;
}

static inline void platform_cpu_halt(void) {
    if (g_platform_ops && g_platform_ops->cpu_halt) {
        g_platform_ops->cpu_halt();
    }
}

static inline void platform_cpu_idle(void) {
    if (g_platform_ops && g_platform_ops->cpu_idle) {
        g_platform_ops->cpu_idle();
    }
}

static inline void platform_invalidate_tlb(void) {
    if (g_platform_ops && g_platform_ops->invalidate_tlb) {
        g_platform_ops->invalidate_tlb();
    }
}

static inline void platform_invalidate_page(uintptr_t vaddr) {
    if (g_platform_ops && g_platform_ops->invalidate_page) {
        g_platform_ops->invalidate_page(vaddr);
    }
}

static inline uint64_t platform_get_ticks(void) {
    if (g_platform_ops && g_platform_ops->get_ticks) {
        return g_platform_ops->get_ticks();
    }
    return 0;
}

static inline void platform_delay_microseconds(uint32_t usec) {
    if (g_platform_ops && g_platform_ops->delay_microseconds) {
        g_platform_ops->delay_microseconds(usec);
    }
}

#ifdef __cplusplus
}
#endif

#endif // KERNEL_HAL_PLATFORM_H