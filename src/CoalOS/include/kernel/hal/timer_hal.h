/**
 * @file timer_hal.h
 * @brief Hardware Timer Abstraction Layer Interface
 * @author CoalOS HAL Implementation
 * @version 1.0
 * 
 * @details Provides a hardware-independent interface for timer operations
 * including system tick generation, delays, and timer callback management.
 */

#ifndef TIMER_HAL_H
#define TIMER_HAL_H

//============================================================================
// Includes
//============================================================================
#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <kernel/core/constants.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//============================================================================
// Timer Types and Constants
//============================================================================

/**
 * @brief Timer callback function type
 * @param context User-provided context pointer
 */
typedef void (*timer_callback_t)(void *context);

/**
 * @brief Timer types supported by the HAL
 */
typedef enum {
    TIMER_TYPE_SYSTEM_TICK = 0,     ///< System tick timer (for scheduler)
    TIMER_TYPE_ONE_SHOT,            ///< One-shot timer
    TIMER_TYPE_PERIODIC,            ///< Periodic timer
    TIMER_TYPE_HIGH_RESOLUTION,     ///< High-resolution timer
    TIMER_TYPE_WATCHDOG            ///< Watchdog timer
} timer_type_t;

/**
 * @brief Timer configuration structure
 */
typedef struct timer_config {
    timer_type_t type;              ///< Timer type
    uint32_t frequency_hz;          ///< Timer frequency in Hz
    timer_callback_t callback;      ///< Callback function
    void *context;                  ///< User context for callback
    bool auto_reload;               ///< Auto-reload for periodic timers
} timer_config_t;

/**
 * @brief Timer information structure
 */
typedef struct timer_info {
    const char *name;               ///< Timer hardware name
    uint32_t max_frequency;         ///< Maximum supported frequency
    uint32_t min_frequency;         ///< Minimum supported frequency
    uint32_t resolution_ns;         ///< Timer resolution in nanoseconds
    bool supports_one_shot;         ///< Supports one-shot mode
    bool supports_periodic;         ///< Supports periodic mode
    uint8_t num_channels;           ///< Number of timer channels
} timer_info_t;

//============================================================================
// Timer HAL Operations
//============================================================================

/**
 * @brief Timer hardware abstraction operations
 */
typedef struct timer_hal_ops {
    // Basic timer operations
    error_t (*init)(void);
    void (*shutdown)(void);
    
    // Timer configuration
    error_t (*configure)(uint8_t timer_id, const timer_config_t *config);
    error_t (*start)(uint8_t timer_id);
    error_t (*stop)(uint8_t timer_id);
    error_t (*reset)(uint8_t timer_id);
    
    // Time measurement
    uint64_t (*get_ticks)(uint8_t timer_id);
    uint64_t (*get_time_us)(uint8_t timer_id);
    uint64_t (*get_time_ns)(uint8_t timer_id);
    
    // Frequency control
    error_t (*set_frequency)(uint8_t timer_id, uint32_t frequency_hz);
    uint32_t (*get_frequency)(uint8_t timer_id);
    
    // Delay functions
    void (*delay_us)(uint32_t microseconds);
    void (*delay_ms)(uint32_t milliseconds);
    void (*busy_wait_us)(uint32_t microseconds);
    
    // Callback management
    error_t (*register_callback)(uint8_t timer_id, timer_callback_t callback, void *context);
    error_t (*unregister_callback)(uint8_t timer_id);
    
    // Timer information
    const timer_info_t *(*get_info)(uint8_t timer_id);
    uint8_t (*get_timer_count)(void);
    
    // Advanced operations
    error_t (*set_period)(uint8_t timer_id, uint32_t period_us);
    uint32_t (*get_period)(uint8_t timer_id);
    bool (*is_running)(uint8_t timer_id);
    
} timer_hal_ops_t;

//============================================================================
// Global Timer HAL Interface
//============================================================================

/**
 * @brief Get timer HAL operations table
 * @return Pointer to timer HAL operations
 */
const timer_hal_ops_t *timer_hal_get_ops(void);

/**
 * @brief Initialize timer HAL
 * @return 0 on success, negative error code on failure
 */
error_t timer_hal_init(void);

/**
 * @brief Shutdown timer HAL
 */
void timer_hal_shutdown(void);

//============================================================================
// Convenience Functions for System Timer
//============================================================================

/**
 * @brief Initialize system tick timer with specified frequency
 * @param frequency_hz Tick frequency in Hz
 * @return 0 on success, negative error code on failure
 */
error_t timer_hal_init_system_tick(uint32_t frequency_hz);

/**
 * @brief Get current system tick count
 * @return Current tick count
 */
uint64_t timer_hal_get_system_ticks(void);

/**
 * @brief Get system uptime in microseconds
 * @return Uptime in microseconds
 */
uint64_t timer_hal_get_uptime_us(void);

/**
 * @brief Register system tick callback
 * @param callback Callback function
 * @param context User context
 * @return 0 on success, negative error code on failure
 */
error_t timer_hal_register_tick_callback(timer_callback_t callback, void *context);

//============================================================================
// Convenience Macros
//============================================================================

#define TIMER_HAL_INIT()                    (timer_hal_get_ops()->init())
#define TIMER_HAL_GET_TICKS(id)             (timer_hal_get_ops()->get_ticks(id))
#define TIMER_HAL_DELAY_US(us)              (timer_hal_get_ops()->delay_us(us))
#define TIMER_HAL_DELAY_MS(ms)              (timer_hal_get_ops()->delay_ms(ms))
#define TIMER_HAL_SET_FREQUENCY(id, freq)   (timer_hal_get_ops()->set_frequency(id, freq))

//============================================================================
// Timer Constants
//============================================================================

#define TIMER_HAL_SYSTEM_TIMER_ID           HAL_TIMER_SYSTEM
#define TIMER_HAL_MAX_TIMERS                HAL_MAX_TIMERS
#define TIMER_HAL_DEFAULT_FREQUENCY         HAL_DEFAULT_TIMER_FREQ
#define TIMER_HAL_MIN_FREQUENCY             1       // 1 Hz
#define TIMER_HAL_MAX_FREQUENCY             1000000 // 1 MHz

//============================================================================
// Architecture-Specific Initialization
//============================================================================

#ifdef __i386__
/**
 * @brief Initialize x86-32 timer HAL (PIT-based)
 * @return 0 on success, negative error code on failure
 */
error_t x86_32_timer_hal_init(void);
#endif

#ifdef __x86_64__
/**
 * @brief Initialize x86-64 timer HAL
 * @return 0 on success, negative error code on failure
 */
error_t x86_64_timer_hal_init(void);
#endif

#ifdef __arm__
/**
 * @brief Initialize ARM timer HAL
 * @return 0 on success, negative error code on failure
 */
error_t arm_timer_hal_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // TIMER_HAL_H