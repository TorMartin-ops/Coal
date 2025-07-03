/**
 * @file cpu_arch.h
 * @brief CPU Architecture Hardware Abstraction Layer Interface
 * @author CoalOS HAL Implementation
 * @version 1.0
 * 
 * @details Provides a hardware-independent interface for CPU architecture
 * specific operations including segment management, interrupt handling,
 * context switching, and system call mechanisms.
 */

#ifndef CPU_ARCH_H
#define CPU_ARCH_H

//============================================================================
// Includes
//============================================================================
#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//============================================================================
// Forward Declarations
//============================================================================
struct isr_frame;
struct pcb;

//============================================================================
// CPU Architecture Types
//============================================================================

/**
 * @brief CPU architecture identification
 */
typedef enum {
    CPU_ARCH_UNKNOWN = 0,
    CPU_ARCH_X86_32,
    CPU_ARCH_X86_64,
    CPU_ARCH_ARM_32,
    CPU_ARCH_ARM_64,
    CPU_ARCH_RISCV_32,
    CPU_ARCH_RISCV_64
} cpu_arch_type_t;

/**
 * @brief CPU feature flags
 */
typedef enum {
    CPU_FEATURE_PAGING          = 0x01,
    CPU_FEATURE_SEGMENTATION    = 0x02,
    CPU_FEATURE_FLOATING_POINT  = 0x04,
    CPU_FEATURE_CACHE_CONTROL   = 0x08,
    CPU_FEATURE_MMU             = 0x10,
    CPU_FEATURE_INTERRUPTS      = 0x20,
    CPU_FEATURE_PRIVILEGED_MODE = 0x40,
    CPU_FEATURE_ATOMIC_OPS      = 0x80
} cpu_feature_flags_t;

/**
 * @brief CPU architecture information
 */
typedef struct cpu_arch_info {
    cpu_arch_type_t type;
    uint32_t features;
    uint32_t word_size;         // 4 for 32-bit, 8 for 64-bit
    uint32_t page_size;         // Default page size
    uint32_t cache_line_size;   // Cache line size
    const char *arch_name;      // Human-readable architecture name
} cpu_arch_info_t;

//============================================================================
// CPU Architecture Operation Functions
//============================================================================

/**
 * @brief CPU architecture operations table
 */
typedef struct cpu_arch_ops {
    // Basic CPU control
    error_t (*init)(void);
    void (*shutdown)(void);
    void (*halt)(void);
    void (*idle)(void);
    
    // Segment management (if supported)
    error_t (*init_segmentation)(void);
    error_t (*setup_kernel_segments)(void);
    error_t (*setup_user_segments)(void);
    
    // Interrupt management
    error_t (*init_interrupts)(void);
    void (*enable_interrupts)(void);
    void (*disable_interrupts)(void);
    bool (*interrupts_enabled)(void);
    
    // Context switching
    void (*context_switch)(uint32_t **old_esp, uint32_t *new_esp);
    error_t (*prepare_user_context)(struct pcb *process);
    void (*enter_user_mode)(uint32_t entry_point, uint32_t user_stack);
    
    // System call mechanism
    error_t (*init_syscall_handler)(void);
    void (*syscall_entry)(void);
    
    // Architecture-specific features
    void (*flush_tlb)(void);
    void (*flush_tlb_page)(uintptr_t vaddr);
    void (*flush_cache)(void);
    uint32_t (*get_cpu_flags)(void);
    void (*set_cpu_flags)(uint32_t flags);
    
    // Exception handling
    error_t (*register_exception_handler)(uint8_t exception, void (*handler)(struct isr_frame *));
    error_t (*register_irq_handler)(uint8_t irq, void (*handler)(struct isr_frame *));
    
    // Memory barrier operations
    void (*memory_barrier)(void);
    void (*read_barrier)(void);
    void (*write_barrier)(void);
    
} cpu_arch_ops_t;

//============================================================================
// Global CPU Architecture Interface
//============================================================================

/**
 * @brief Get CPU architecture information
 * @return Pointer to CPU architecture information structure
 */
const cpu_arch_info_t *cpu_arch_get_info(void);

/**
 * @brief Get CPU architecture operations table
 * @return Pointer to CPU architecture operations
 */
const cpu_arch_ops_t *cpu_arch_get_ops(void);

/**
 * @brief Initialize CPU architecture abstraction layer
 * @return 0 on success, negative error code on failure
 */
error_t cpu_arch_init(void);

//============================================================================
// Convenience Macros for Common Operations
//============================================================================

#define CPU_ARCH_INIT()             (cpu_arch_get_ops()->init())
#define CPU_ARCH_HALT()             (cpu_arch_get_ops()->halt())
#define CPU_ARCH_IDLE()             (cpu_arch_get_ops()->idle())
#define CPU_ARCH_ENABLE_INTERRUPTS() (cpu_arch_get_ops()->enable_interrupts())
#define CPU_ARCH_DISABLE_INTERRUPTS() (cpu_arch_get_ops()->disable_interrupts())
#define CPU_ARCH_INTERRUPTS_ENABLED() (cpu_arch_get_ops()->interrupts_enabled())
#define CPU_ARCH_FLUSH_TLB()        (cpu_arch_get_ops()->flush_tlb())
#define CPU_ARCH_MEMORY_BARRIER()   (cpu_arch_get_ops()->memory_barrier())

//============================================================================
// Architecture-Specific Initialization
//============================================================================

#ifdef __i386__
/**
 * @brief Initialize x86-32 CPU architecture support
 * @return 0 on success, negative error code on failure
 */
error_t x86_32_cpu_arch_init(void);
#endif

#ifdef __x86_64__
/**
 * @brief Initialize x86-64 CPU architecture support
 * @return 0 on success, negative error code on failure
 */
error_t x86_64_cpu_arch_init(void);
#endif

#ifdef __arm__
/**
 * @brief Initialize ARM CPU architecture support
 * @return 0 on success, negative error code on failure
 */
error_t arm_cpu_arch_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // CPU_ARCH_H