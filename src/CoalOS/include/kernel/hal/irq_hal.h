/**
 * @file irq_hal.h
 * @brief Interrupt Controller Hardware Abstraction Layer Interface
 * @author CoalOS HAL Implementation
 * @version 1.0
 * 
 * @details Provides a hardware-independent interface for interrupt controller
 * operations including IRQ management, priority control, and interrupt routing.
 */

#ifndef IRQ_HAL_H
#define IRQ_HAL_H

//============================================================================
// Includes
//============================================================================
#include <kernel/core/types.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//============================================================================
// Forward Declarations
//============================================================================
struct isr_frame;

//============================================================================
// IRQ Types and Constants
//============================================================================

/**
 * @brief IRQ handler function type
 * @param irq IRQ number that triggered
 * @param frame Interrupt frame with CPU state
 * @param context User-provided context
 */
typedef void (*irq_handler_t)(uint8_t irq, struct isr_frame *frame, void *context);

/**
 * @brief Interrupt controller types
 */
typedef enum {
    IRQ_CONTROLLER_PIC_8259 = 0,    ///< Intel 8259 PIC
    IRQ_CONTROLLER_APIC,            ///< Advanced PIC
    IRQ_CONTROLLER_GIC,             ///< ARM Generic Interrupt Controller
    IRQ_CONTROLLER_PLIC,            ///< RISC-V Platform Level Interrupt Controller
    IRQ_CONTROLLER_NVIC             ///< ARM Nested Vector Interrupt Controller
} irq_controller_type_t;

/**
 * @brief IRQ trigger modes
 */
typedef enum {
    IRQ_TRIGGER_EDGE = 0,           ///< Edge-triggered
    IRQ_TRIGGER_LEVEL               ///< Level-triggered
} irq_trigger_mode_t;

/**
 * @brief IRQ polarity
 */
typedef enum {
    IRQ_POLARITY_ACTIVE_HIGH = 0,   ///< Active high
    IRQ_POLARITY_ACTIVE_LOW         ///< Active low
} irq_polarity_t;

/**
 * @brief IRQ configuration structure
 */
typedef struct irq_config {
    irq_trigger_mode_t trigger_mode;    ///< Trigger mode
    irq_polarity_t polarity;            ///< Signal polarity
    uint8_t priority;                   ///< Interrupt priority (0 = highest)
    bool enabled;                       ///< Initially enabled
} irq_config_t;

/**
 * @brief Interrupt controller information
 */
typedef struct irq_controller_info {
    irq_controller_type_t type;         ///< Controller type
    const char *name;                   ///< Controller name
    uint8_t max_irqs;                   ///< Maximum number of IRQs
    uint8_t priority_levels;            ///< Number of priority levels
    bool supports_nmi;                  ///< Supports Non-Maskable Interrupts
    bool supports_priority;             ///< Supports priority levels
    bool supports_affinity;             ///< Supports CPU affinity
} irq_controller_info_t;

//============================================================================
// IRQ HAL Operations
//============================================================================

/**
 * @brief Interrupt controller hardware abstraction operations
 */
typedef struct irq_hal_ops {
    // Basic controller operations
    error_t (*init)(void);
    void (*shutdown)(void);
    
    // IRQ configuration
    error_t (*configure_irq)(uint8_t irq, const irq_config_t *config);
    error_t (*enable_irq)(uint8_t irq);
    error_t (*disable_irq)(uint8_t irq);
    bool (*is_irq_enabled)(uint8_t irq);
    
    // Handler management
    error_t (*register_handler)(uint8_t irq, irq_handler_t handler, void *context);
    error_t (*unregister_handler)(uint8_t irq);
    
    // Interrupt control
    void (*send_eoi)(uint8_t irq);          ///< Send End of Interrupt
    void (*mask_all_irqs)(void);
    void (*unmask_all_irqs)(void);
    uint32_t (*get_irq_mask)(void);
    void (*set_irq_mask)(uint32_t mask);
    
    // Priority management
    error_t (*set_irq_priority)(uint8_t irq, uint8_t priority);
    uint8_t (*get_irq_priority)(uint8_t irq);
    
    // Status and information
    bool (*is_irq_pending)(uint8_t irq);
    uint8_t (*get_current_irq)(void);
    const irq_controller_info_t *(*get_info)(void);
    
    // Advanced features
    error_t (*set_irq_affinity)(uint8_t irq, uint32_t cpu_mask);
    uint32_t (*get_irq_affinity)(uint8_t irq);
    error_t (*trigger_software_irq)(uint8_t irq, uint32_t cpu_mask);
    
    // Power management
    void (*suspend)(void);
    void (*resume)(void);
    
} irq_hal_ops_t;

//============================================================================
// Global IRQ HAL Interface
//============================================================================

/**
 * @brief Get IRQ HAL operations table
 * @return Pointer to IRQ HAL operations
 */
const irq_hal_ops_t *irq_hal_get_ops(void);

/**
 * @brief Initialize IRQ HAL
 * @return 0 on success, negative error code on failure
 */
error_t irq_hal_init(void);

/**
 * @brief Shutdown IRQ HAL
 */
void irq_hal_shutdown(void);

//============================================================================
// Convenience Functions
//============================================================================

/**
 * @brief Enable an IRQ with default configuration
 * @param irq IRQ number to enable
 * @return 0 on success, negative error code on failure
 */
error_t irq_hal_enable(uint8_t irq);

/**
 * @brief Disable an IRQ
 * @param irq IRQ number to disable
 * @return 0 on success, negative error code on failure
 */
error_t irq_hal_disable(uint8_t irq);

/**
 * @brief Register an IRQ handler with default configuration
 * @param irq IRQ number
 * @param handler Handler function
 * @param context User context for handler
 * @return 0 on success, negative error code on failure
 */
error_t irq_hal_register(uint8_t irq, irq_handler_t handler, void *context);

/**
 * @brief Unregister an IRQ handler
 * @param irq IRQ number
 * @return 0 on success, negative error code on failure
 */
error_t irq_hal_unregister(uint8_t irq);

/**
 * @brief Send End of Interrupt signal
 * @param irq IRQ number that was handled
 */
void irq_hal_eoi(uint8_t irq);

//============================================================================
// Convenience Macros
//============================================================================

#define IRQ_HAL_INIT()                  (irq_hal_get_ops()->init())
#define IRQ_HAL_ENABLE(irq)             (irq_hal_get_ops()->enable_irq(irq))
#define IRQ_HAL_DISABLE(irq)            (irq_hal_get_ops()->disable_irq(irq))
#define IRQ_HAL_EOI(irq)                (irq_hal_get_ops()->send_eoi(irq))
#define IRQ_HAL_MASK_ALL()              (irq_hal_get_ops()->mask_all_irqs())
#define IRQ_HAL_UNMASK_ALL()            (irq_hal_get_ops()->unmask_all_irqs())

//============================================================================
// Standard IRQ Numbers (x86 compatible)
//============================================================================

#define IRQ_TIMER                       0       ///< System timer (PIT)
#define IRQ_KEYBOARD                    1       ///< Keyboard
#define IRQ_CASCADE                     2       ///< PIC cascade (not used)
#define IRQ_SERIAL_2_4                  3       ///< Serial ports 2 & 4
#define IRQ_SERIAL_1_3                  4       ///< Serial ports 1 & 3
#define IRQ_PARALLEL_2                  5       ///< Parallel port 2 / Sound card
#define IRQ_FLOPPY                      6       ///< Floppy disk controller
#define IRQ_PARALLEL_1                  7       ///< Parallel port 1
#define IRQ_RTC                         8       ///< Real-time clock
#define IRQ_ACPI                        9       ///< ACPI
#define IRQ_AVAILABLE_1                 10      ///< Available
#define IRQ_AVAILABLE_2                 11      ///< Available
#define IRQ_MOUSE                       12      ///< PS/2 Mouse
#define IRQ_COPROCESSOR                 13      ///< Math coprocessor
#define IRQ_ATA_PRIMARY                 14      ///< Primary ATA
#define IRQ_ATA_SECONDARY               15      ///< Secondary ATA

#define IRQ_MAX_STANDARD                16      ///< Maximum standard IRQ number

//============================================================================
// Architecture-Specific Initialization
//============================================================================

#ifdef __i386__
/**
 * @brief Initialize x86-32 IRQ HAL (8259 PIC-based)
 * @return 0 on success, negative error code on failure
 */
error_t x86_32_irq_hal_init(void);
#endif

#ifdef __x86_64__
/**
 * @brief Initialize x86-64 IRQ HAL (APIC-based)
 * @return 0 on success, negative error code on failure
 */
error_t x86_64_irq_hal_init(void);
#endif

#ifdef __arm__
/**
 * @brief Initialize ARM IRQ HAL (GIC/NVIC-based)
 * @return 0 on success, negative error code on failure
 */
error_t arm_irq_hal_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // IRQ_HAL_H