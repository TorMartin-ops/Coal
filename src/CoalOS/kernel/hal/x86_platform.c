/**
 * @file x86_platform.c
 * @brief x86 platform implementation following Single Responsibility Principle
 * 
 * This module is responsible ONLY for:
 * - x86-specific hardware abstraction
 * - Platform operations implementation
 * - Hardware capability detection
 */

#define LOG_MODULE "x86_platform"

#include <kernel/hal/platform.h>
#include <kernel/interfaces/logger.h>
#include <kernel/lib/port_io.h>
#include <kernel/cpu/gdt.h>
#include <kernel/cpu/idt.h>
#include <kernel/memory/paging.h>

// x86 platform information
static platform_info_t x86_platform_info = {
    .name = "x86-32",
    .vendor = "Generic x86",
    .version = "1.0",
    .capabilities = 0,
    .cpu_count = 1,
    .total_memory = 0,
    .page_size = 4096,
};

/**
 * @brief x86 platform initialization
 */
static error_t x86_platform_init(void) {
    LOGGER_INFO(LOG_MODULE, "Initializing x86 platform");
    
    // Detect capabilities
    x86_platform_info.capabilities = 0;
    
    // Check for NX bit support (simplified detection)
    extern bool g_nx_supported;
    if (g_nx_supported) {
        x86_platform_info.capabilities |= PLATFORM_CAP_NX_BIT;
    }
    
    // TODO: Add CPUID-based capability detection
    // TODO: Add memory size detection
    
    LOGGER_INFO(LOG_MODULE, "x86 platform initialized with capabilities: %#x", 
               x86_platform_info.capabilities);
    
    return E_SUCCESS;
}

/**
 * @brief x86 platform cleanup
 */
static void x86_platform_cleanup(void) {
    LOGGER_INFO(LOG_MODULE, "x86 platform cleaned up");
}

/**
 * @brief Get x86 platform information
 */
static void x86_platform_get_info(platform_info_t *info) {
    if (info) {
        *info = x86_platform_info;
    }
}

/**
 * @brief Enable interrupts (x86 STI)
 */
static void x86_interrupts_enable(void) {
    asm volatile ("sti" ::: "memory");
}

/**
 * @brief Disable interrupts (x86 CLI)
 */
static void x86_interrupts_disable(void) {
    asm volatile ("cli" ::: "memory");
}

/**
 * @brief Check if interrupts are enabled
 */
static bool x86_interrupts_enabled(void) {
    uint32_t flags;
    asm volatile ("pushfl; popl %0" : "=r"(flags) :: "memory");
    return (flags & (1 << 9)) != 0; // IF flag is bit 9
}

/**
 * @brief Halt CPU (x86 HLT)
 */
static void x86_cpu_halt(void) {
    asm volatile ("cli; hlt" ::: "memory");
}

/**
 * @brief Put CPU in idle state
 */
static void x86_cpu_idle(void) {
    asm volatile ("hlt" ::: "memory");
}

/**
 * @brief Invalidate entire TLB
 */
static void x86_invalidate_tlb(void) {
    uint32_t cr3;
    asm volatile ("movl %%cr3, %0" : "=r"(cr3) :: "memory");
    asm volatile ("movl %0, %%cr3" :: "r"(cr3) : "memory");
}

/**
 * @brief Invalidate single page in TLB
 */
static void x86_invalidate_page(uintptr_t vaddr) {
    asm volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/**
 * @brief Flush CPU cache
 */
static void x86_flush_cache(void) {
    asm volatile ("wbinvd" ::: "memory");
}

/**
 * @brief Get CPU tick count (using TSC if available)
 */
static uint64_t x86_get_ticks(void) {
    uint64_t tsc;
    asm volatile ("rdtsc" : "=A"(tsc));
    return tsc;
}

/**
 * @brief Delay for specified microseconds
 */
static void x86_delay_microseconds(uint32_t usec) {
    // Simple delay using port I/O (very crude, for compatibility)
    for (uint32_t i = 0; i < usec * 10; i++) {
        inb(0x80); // Dummy I/O to create delay
    }
}

// x86 platform operations structure
static platform_operations_t x86_platform_ops = {
    .init = x86_platform_init,
    .cleanup = x86_platform_cleanup,
    .get_info = x86_platform_get_info,
    .interrupts_enable = x86_interrupts_enable,
    .interrupts_disable = x86_interrupts_disable,
    .interrupts_enabled = x86_interrupts_enabled,
    .cpu_halt = x86_cpu_halt,
    .cpu_idle = x86_cpu_idle,
    .invalidate_tlb = x86_invalidate_tlb,
    .invalidate_page = x86_invalidate_page,
    .flush_cache = x86_flush_cache,
    .get_ticks = x86_get_ticks,
    .delay_microseconds = x86_delay_microseconds,
    .name = "x86-32",
};

/**
 * @brief Get x86 platform operations
 */
platform_operations_t *get_x86_platform_operations(void) {
    return &x86_platform_ops;
}

/**
 * @brief Initialize x86 platform and set as current
 */
error_t x86_platform_setup(void) {
    platform_set_operations(&x86_platform_ops);
    return platform_init();
}