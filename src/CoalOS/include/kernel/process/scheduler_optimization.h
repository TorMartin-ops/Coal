/**
 * @file scheduler_optimization.h
 * @brief Scheduler Optimization Enhancements for Coal OS
 * @version 1.0
 * @author Performance optimization
 * 
 * @details Provides enhanced scheduler features including:
 * - Bitmap-based priority queue for O(1) task selection
 * - CPU affinity support (preparation for SMP)
 * - Dynamic priority adjustment
 * - Load balancing statistics
 */

#ifndef SCHEDULER_OPTIMIZATION_H
#define SCHEDULER_OPTIMIZATION_H

#include <kernel/process/scheduler.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

//============================================================================
// Configuration
//============================================================================

#define SCHED_BITMAP_SIZE       (SCHED_PRIORITY_LEVELS / 32 + 1)
#define SCHED_LOAD_HISTORY_SIZE 8
#define SCHED_BOOST_THRESHOLD   10  // Boost priority after N ticks of waiting

//============================================================================
// Bitmap Operations
//============================================================================

/**
 * @brief Priority bitmap for O(1) priority level lookup
 */
typedef struct {
    uint32_t bitmap[SCHED_BITMAP_SIZE];
} priority_bitmap_t;

/**
 * @brief Set priority bit in bitmap
 * @param bitmap Priority bitmap
 * @param priority Priority level to set
 */
static inline void bitmap_set_priority(priority_bitmap_t *bitmap, uint8_t priority) {
    if (priority < SCHED_PRIORITY_LEVELS) {
        bitmap->bitmap[priority / 32] |= (1U << (priority % 32));
    }
}

/**
 * @brief Clear priority bit in bitmap
 * @param bitmap Priority bitmap
 * @param priority Priority level to clear
 */
static inline void bitmap_clear_priority(priority_bitmap_t *bitmap, uint8_t priority) {
    if (priority < SCHED_PRIORITY_LEVELS) {
        bitmap->bitmap[priority / 32] &= ~(1U << (priority % 32));
    }
}

/**
 * @brief Find highest priority with tasks
 * @param bitmap Priority bitmap
 * @return Highest priority level with tasks, or -1 if none
 */
static inline int bitmap_find_first_set(priority_bitmap_t *bitmap) {
    for (int i = 0; i < SCHED_BITMAP_SIZE; i++) {
        if (bitmap->bitmap[i] != 0) {
            // Find first set bit using built-in function
            return i * 32 + __builtin_ctz(bitmap->bitmap[i]);
        }
    }
    return -1;
}

//============================================================================
// Load Tracking
//============================================================================

/**
 * @brief Load tracking structure for scheduler statistics
 */
typedef struct {
    uint32_t total_tasks;
    uint32_t runnable_tasks;
    uint32_t blocked_tasks;
    uint32_t load_history[SCHED_LOAD_HISTORY_SIZE];
    uint32_t history_index;
    uint32_t average_load;
} scheduler_load_t;

//============================================================================
// Dynamic Priority
//============================================================================

/**
 * @brief Dynamic priority adjustment information
 */
typedef struct {
    uint32_t wait_ticks;        // Ticks spent waiting in ready queue
    uint32_t total_run_ticks;   // Total ticks spent running
    uint32_t boost_count;       // Number of priority boosts received
    bool is_interactive;        // Interactive task flag
} task_stats_t;

//============================================================================
// Optimization API
//============================================================================

/**
 * @brief Initialize scheduler optimizations
 */
void scheduler_opt_init(void);

/**
 * @brief Update priority bitmap when task is enqueued
 * @param priority Priority level that now has tasks
 */
void scheduler_opt_mark_priority_active(uint8_t priority);

/**
 * @brief Update priority bitmap when queue becomes empty
 * @param priority Priority level that is now empty
 */
void scheduler_opt_mark_priority_empty(uint8_t priority);

/**
 * @brief Get next task using optimized selection
 * @return Next task to run or NULL
 */
tcb_t* scheduler_opt_select_next_task(void);

/**
 * @brief Update load statistics
 */
void scheduler_opt_update_load_stats(void);

/**
 * @brief Check if task should receive priority boost
 * @param task Task to check
 * @return true if task should be boosted
 */
bool scheduler_opt_should_boost_priority(tcb_t *task);

/**
 * @brief Apply dynamic priority boost
 * @param task Task to boost
 */
void scheduler_opt_boost_priority(tcb_t *task);

/**
 * @brief Reset task priority boost
 * @param task Task to reset
 */
void scheduler_opt_reset_boost(tcb_t *task);

/**
 * @brief Get current scheduler load average
 * @return Load average (0-100)
 */
uint32_t scheduler_opt_get_load_average(void);

/**
 * @brief Check if scheduler is under heavy load
 * @return true if load is high
 */
bool scheduler_opt_is_high_load(void);

/**
 * @brief Get priority queue statistics
 * @param stats Buffer to fill with statistics
 */
void scheduler_opt_get_queue_stats(scheduler_load_t *stats);

#endif // SCHEDULER_OPTIMIZATION_H