/**
 * @file x86_32_timer_hal.c
 * @brief x86-32 Timer HAL Implementation (PIT-based)
 * @author CoalOS HAL Implementation
 * @version 1.0
 * 
 * @details Implements timer abstraction for x86-32 using the PIT (8254)
 * and potentially other x86 timer sources like APIC timer and TSC.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/hal/timer_hal.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/lib/port_io.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <libc/string.h>

//============================================================================
// x86-32 Timer Hardware Constants
//============================================================================

#define PIT_FREQUENCY_HZ            1193182     // PIT base frequency
#define PIT_MAX_FREQUENCY           1193182     // Maximum PIT frequency
#define PIT_MIN_FREQUENCY           18          // Minimum practical frequency
#define PIT_TIMER_RESOLUTION_NS     838         // ~838ns resolution

#define X86_32_MAX_TIMERS           1           // Only PIT for now
#define X86_32_SYSTEM_TIMER_ID      0           // PIT is system timer

//============================================================================
// x86-32 Timer State
//============================================================================

typedef struct x86_32_timer_state {
    bool initialized;
    bool running;
    uint32_t frequency;
    uint64_t tick_count;
    timer_callback_t callback;
    void *callback_context;
} x86_32_timer_state_t;

static x86_32_timer_state_t x86_32_timers[X86_32_MAX_TIMERS];
static bool x86_32_timer_hal_initialized = false;

//============================================================================
// x86-32 Timer Information
//============================================================================

static const timer_info_t x86_32_pit_info = {
    .name = "Intel 8254 PIT",
    .max_frequency = PIT_MAX_FREQUENCY,
    .min_frequency = PIT_MIN_FREQUENCY,
    .resolution_ns = PIT_TIMER_RESOLUTION_NS,
    .supports_one_shot = false,     // PIT is primarily periodic
    .supports_periodic = true,
    .num_channels = 3               // PIT has 3 channels, we use channel 0
};

//============================================================================
// External PIT Functions
//============================================================================

// These should be available from the existing PIT driver
extern void init_pit(void);
extern uint32_t get_pit_ticks(void);
// Note: PIT frequency setting and callbacks will need to be implemented

//============================================================================
// x86-32 Timer Operations Implementation
//============================================================================

static error_t x86_32_timer_init(void)
{
    serial_write("[x86-32 Timer HAL] Initializing timer subsystem...\n");
    
    // Initialize timer state
    memset(x86_32_timers, 0, sizeof(x86_32_timers));
    
    // Initialize PIT as system timer
    init_pit();
    
    // Set up system timer state
    x86_32_timers[X86_32_SYSTEM_TIMER_ID].initialized = true;
    x86_32_timers[X86_32_SYSTEM_TIMER_ID].frequency = TIMER_HAL_DEFAULT_FREQUENCY;
    x86_32_timers[X86_32_SYSTEM_TIMER_ID].running = true;
    
    serial_write("[x86-32 Timer HAL] Timer subsystem initialized\n");
    return E_SUCCESS;
}

static void x86_32_timer_shutdown(void)
{
    serial_write("[x86-32 Timer HAL] Shutting down timer subsystem\n");
    
    // Stop all timers
    for (int i = 0; i < X86_32_MAX_TIMERS; i++) {
        x86_32_timers[i].running = false;
        x86_32_timers[i].callback = NULL;
    }
}

static error_t x86_32_timer_configure(uint8_t timer_id, const timer_config_t *config)
{
    if (timer_id >= X86_32_MAX_TIMERS || !config) {
        return E_INVAL;
    }
    
    if (timer_id == X86_32_SYSTEM_TIMER_ID) {
        // Configure PIT
        if (config->frequency_hz < PIT_MIN_FREQUENCY || 
            config->frequency_hz > PIT_MAX_FREQUENCY) {
            return E_INVAL;
        }
        
        // Note: PIT frequency setting not implemented yet
        x86_32_timers[timer_id].frequency = config->frequency_hz;
        x86_32_timers[timer_id].callback = config->callback;
        x86_32_timers[timer_id].callback_context = config->context;
        
        return E_SUCCESS;
    }
    
    return E_NOTSUP; // Other timers not implemented yet
}

static error_t x86_32_timer_start(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    if (!x86_32_timers[timer_id].initialized) {
        return E_NOTSUP;
    }
    
    x86_32_timers[timer_id].running = true;
    return E_SUCCESS;
}

static error_t x86_32_timer_stop(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    x86_32_timers[timer_id].running = false;
    return E_SUCCESS;
}

static error_t x86_32_timer_reset(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    x86_32_timers[timer_id].tick_count = 0;
    return E_SUCCESS;
}

static uint64_t x86_32_timer_get_ticks(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return 0;
    }
    
    if (timer_id == X86_32_SYSTEM_TIMER_ID) {
        return (uint64_t)get_pit_ticks();
    }
    
    return x86_32_timers[timer_id].tick_count;
}

static uint64_t x86_32_timer_get_time_us(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return 0;
    }
    
    uint32_t ticks = (uint32_t)x86_32_timer_get_ticks(timer_id);
    uint32_t frequency = x86_32_timers[timer_id].frequency;
    
    if (frequency == 0) {
        return 0;
    }
    
    // Convert ticks to microseconds: (ticks * 1000000) / frequency
    // Use 32-bit arithmetic to avoid __udivdi3
    return (uint64_t)((ticks * 1000UL) / frequency) * 1000UL;
}

static uint64_t x86_32_timer_get_time_ns(uint8_t timer_id)
{
    return x86_32_timer_get_time_us(timer_id) * 1000ULL;
}

static error_t x86_32_timer_set_frequency(uint8_t timer_id, uint32_t frequency_hz)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    if (timer_id == X86_32_SYSTEM_TIMER_ID) {
        if (frequency_hz < PIT_MIN_FREQUENCY || frequency_hz > PIT_MAX_FREQUENCY) {
            return E_INVAL;
        }
        
        // Note: PIT frequency setting not implemented yet
        x86_32_timers[timer_id].frequency = frequency_hz;
        return E_SUCCESS;
    }
    
    return E_NOTSUP;
}

static uint32_t x86_32_timer_get_frequency(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return 0;
    }
    
    return x86_32_timers[timer_id].frequency;
}

static void x86_32_timer_delay_us(uint32_t microseconds)
{
    // Simple busy-wait delay using PIT
    uint64_t start_time = x86_32_timer_get_time_us(X86_32_SYSTEM_TIMER_ID);
    uint64_t target_time = start_time + microseconds;
    
    while (x86_32_timer_get_time_us(X86_32_SYSTEM_TIMER_ID) < target_time) {
        __asm__ volatile ("pause");
    }
}

static void x86_32_timer_delay_ms(uint32_t milliseconds)
{
    x86_32_timer_delay_us(milliseconds * 1000);
}

static void x86_32_timer_busy_wait_us(uint32_t microseconds)
{
    // Same as delay_us for now
    x86_32_timer_delay_us(microseconds);
}

static error_t x86_32_timer_register_callback(uint8_t timer_id, timer_callback_t callback, void *context)
{
    if (timer_id >= X86_32_MAX_TIMERS || !callback) {
        return E_INVAL;
    }
    
    x86_32_timers[timer_id].callback = callback;
    x86_32_timers[timer_id].callback_context = context;
    
    // For PIT, register with the PIT driver
    if (timer_id == X86_32_SYSTEM_TIMER_ID) {
        // We need to create a wrapper that calls our callback
        // For now, just store the callback
    }
    
    return E_SUCCESS;
}

static error_t x86_32_timer_unregister_callback(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    x86_32_timers[timer_id].callback = NULL;
    x86_32_timers[timer_id].callback_context = NULL;
    
    return E_SUCCESS;
}

static const timer_info_t *x86_32_timer_get_info(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return NULL;
    }
    
    if (timer_id == X86_32_SYSTEM_TIMER_ID) {
        return &x86_32_pit_info;
    }
    
    return NULL;
}

static uint8_t x86_32_timer_get_timer_count(void)
{
    return X86_32_MAX_TIMERS;
}

static error_t x86_32_timer_set_period(uint8_t timer_id, uint32_t period_us)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return E_INVAL;
    }
    
    if (period_us == 0) {
        return E_INVAL;
    }
    
    // Convert period to frequency: frequency = 1000000 / period_us
    uint32_t frequency = 1000000 / period_us;
    return x86_32_timer_set_frequency(timer_id, frequency);
}

static uint32_t x86_32_timer_get_period(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return 0;
    }
    
    uint32_t frequency = x86_32_timers[timer_id].frequency;
    if (frequency == 0) {
        return 0;
    }
    
    // Convert frequency to period: period_us = 1000000 / frequency
    return 1000000 / frequency;
}

static bool x86_32_timer_is_running(uint8_t timer_id)
{
    if (timer_id >= X86_32_MAX_TIMERS) {
        return false;
    }
    
    return x86_32_timers[timer_id].running;
}

//============================================================================
// x86-32 Timer Operations Table
//============================================================================

static const timer_hal_ops_t x86_32_timer_ops = {
    .init = x86_32_timer_init,
    .shutdown = x86_32_timer_shutdown,
    
    .configure = x86_32_timer_configure,
    .start = x86_32_timer_start,
    .stop = x86_32_timer_stop,
    .reset = x86_32_timer_reset,
    
    .get_ticks = x86_32_timer_get_ticks,
    .get_time_us = x86_32_timer_get_time_us,
    .get_time_ns = x86_32_timer_get_time_ns,
    
    .set_frequency = x86_32_timer_set_frequency,
    .get_frequency = x86_32_timer_get_frequency,
    
    .delay_us = x86_32_timer_delay_us,
    .delay_ms = x86_32_timer_delay_ms,
    .busy_wait_us = x86_32_timer_busy_wait_us,
    
    .register_callback = x86_32_timer_register_callback,
    .unregister_callback = x86_32_timer_unregister_callback,
    
    .get_info = x86_32_timer_get_info,
    .get_timer_count = x86_32_timer_get_timer_count,
    
    .set_period = x86_32_timer_set_period,
    .get_period = x86_32_timer_get_period,
    .is_running = x86_32_timer_is_running
};

//============================================================================
// Global Interface Implementation
//============================================================================

const timer_hal_ops_t *timer_hal_get_ops(void)
{
    return &x86_32_timer_ops;
}

error_t timer_hal_init(void)
{
    if (x86_32_timer_hal_initialized) {
        return 0;
    }
    
    serial_write("[Timer HAL] Initializing x86-32 timer HAL\n");
    
    error_t result = x86_32_timer_init();
    if (result == E_SUCCESS) {
        x86_32_timer_hal_initialized = true;
        serial_write("[Timer HAL] x86-32 timer HAL initialized successfully\n");
    } else {
        serial_write("[Timer HAL] Failed to initialize x86-32 timer HAL\n");
    }
    
    return result;
}

void timer_hal_shutdown(void)
{
    if (x86_32_timer_hal_initialized) {
        x86_32_timer_shutdown();
        x86_32_timer_hal_initialized = false;
    }
}

//============================================================================
// Convenience Functions
//============================================================================

error_t timer_hal_init_system_tick(uint32_t frequency_hz)
{
    timer_config_t config = {
        .type = TIMER_TYPE_SYSTEM_TICK,
        .frequency_hz = frequency_hz,
        .callback = NULL,
        .context = NULL,
        .auto_reload = true
    };
    
    return x86_32_timer_configure(TIMER_HAL_SYSTEM_TIMER_ID, &config);
}

uint64_t timer_hal_get_system_ticks(void)
{
    return x86_32_timer_get_ticks(TIMER_HAL_SYSTEM_TIMER_ID);
}

uint64_t timer_hal_get_uptime_us(void)
{
    return x86_32_timer_get_time_us(TIMER_HAL_SYSTEM_TIMER_ID);
}

error_t timer_hal_register_tick_callback(timer_callback_t callback, void *context)
{
    return x86_32_timer_register_callback(TIMER_HAL_SYSTEM_TIMER_ID, callback, context);
}

error_t x86_32_timer_hal_init(void)
{
    return timer_hal_init();
}