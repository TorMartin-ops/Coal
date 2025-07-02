#include <kernel/sync/spinlock.h>
#include <kernel/interfaces/logger.h> // For logging

#define LOG_MODULE "spinlock"

/**
 * @brief Initializes a spinlock to the unlocked state with performance tracking.
 */
void spinlock_init(spinlock_t *lock) {
    if (lock) {
        lock->locked = 0;
        lock->contention_count = 0;
        lock->total_acquisitions = 0;
        lock->adaptive_threshold = 100; // Default adaptive threshold
    }
}

/**
 * @brief Optimized spinlock acquisition with exponential backoff and adaptive spinning.
 * Uses read-before-test-and-set optimization to reduce cache coherency traffic.
 */
uintptr_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uintptr_t flags = local_irq_save(); // Disable interrupts, save state
    if (!lock) {
        LOGGER_ERROR(LOG_MODULE, "Trying to acquire NULL lock!");
        return flags; // Return saved flags even on error
    }

    // Increment total acquisitions for statistics
    __atomic_fetch_add(&lock->total_acquisitions, 1, __ATOMIC_RELAXED);

    // Fast path: try to acquire immediately without spinning
    if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        return flags; // Lock acquired immediately
    }

    // Slow path: lock was contended, track contention
    __atomic_fetch_add(&lock->contention_count, 1, __ATOMIC_RELAXED);

    // Optimized spinning with exponential backoff
    uint32_t backoff_cycles = 1;
    uint32_t spin_count = 0;
    const uint32_t max_backoff = 256;
    const uint32_t adaptive_limit = lock->adaptive_threshold;

    while (1) {
        // Read-before-test-and-set optimization
        // First check if lock is available without atomic operation (reduces cache traffic)
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            // Exponential backoff with pause instruction
            for (uint32_t i = 0; i < backoff_cycles; i++) {
                asm volatile ("pause" ::: "memory");
            }
            
            // Increase backoff time exponentially, but cap it
            if (backoff_cycles < max_backoff) {
                backoff_cycles <<= 1; // Double the backoff
            }
            
            spin_count++;
            
            // Adaptive spinning: if we've spun too long, yield to other processes
            if (spin_count > adaptive_limit) {
                // TODO: Implement yield() or schedule() call for cooperative multitasking
                // For now, just reset backoff to avoid excessive spinning
                backoff_cycles = 1;
                spin_count = 0;
            }
        }

        // Lock appears to be free, try to acquire it atomically
        if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
            break; // Successfully acquired the lock
        }
        
        // Lock was taken between our read and test-and-set, continue spinning
        // Reset backoff to be more aggressive since lock is changing hands
        backoff_cycles = (backoff_cycles > 4) ? (backoff_cycles >> 1) : 1;
    }

    return flags; // Return previous interrupt state
}

/**
 * @brief Releases the spinlock and restores the previous interrupt state.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags) {
    if (!lock) {
        terminal_write("[Spinlock] Error: Trying to release NULL lock!\n");
        // Restore interrupts anyway? Or panic?
        local_irq_restore(flags);
        return;
    }

    // Atomically clear the lock flag.
    // __ATOMIC_RELEASE ensures memory operations before are not reordered after.
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);

    local_irq_restore(flags); // Restore previous interrupt state
}

//============================================================================
// Spinlock Performance Optimizations and Statistics
//============================================================================

/**
 * @brief Tries to acquire spinlock without spinning (non-blocking).
 */
uintptr_t spinlock_try_acquire_irqsave(spinlock_t *lock) {
    if (!lock) return 0;
    
    uintptr_t flags = local_irq_save();
    
    // Try to acquire lock without spinning
    if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // Successfully acquired
        __atomic_fetch_add(&lock->total_acquisitions, 1, __ATOMIC_RELAXED);
        return flags;
    }
    
    // Lock was already held, restore interrupts and return failure
    local_irq_restore(flags);
    return 0; // Indicates failure to acquire
}

/**
 * @brief Get spinlock performance statistics.
 */
void spinlock_get_stats(spinlock_t *lock, uint32_t *contention_count, uint32_t *total_acquisitions) {
    if (!lock) return;
    
    if (contention_count) {
        *contention_count = __atomic_load_n(&lock->contention_count, __ATOMIC_RELAXED);
    }
    if (total_acquisitions) {
        *total_acquisitions = __atomic_load_n(&lock->total_acquisitions, __ATOMIC_RELAXED);
    }
}

/**
 * @brief Reset spinlock performance statistics.
 */
void spinlock_reset_stats(spinlock_t *lock) {
    if (!lock) return;
    
    __atomic_store_n(&lock->contention_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->total_acquisitions, 0, __ATOMIC_RELAXED);
}

/**
 * @brief Adjust adaptive spinning threshold based on contention patterns.
 */
void spinlock_tune_adaptive_threshold(spinlock_t *lock) {
    if (!lock) return;
    
    uint32_t contentions = __atomic_load_n(&lock->contention_count, __ATOMIC_RELAXED);
    uint32_t acquisitions = __atomic_load_n(&lock->total_acquisitions, __ATOMIC_RELAXED);
    
    if (acquisitions == 0) return;
    
    // Calculate contention ratio (contentions per 100 acquisitions)
    uint32_t contention_ratio = (contentions * 100) / acquisitions;
    
    // Adjust threshold based on contention ratio
    if (contention_ratio > 50) {
        // High contention - reduce spinning, yield sooner
        lock->adaptive_threshold = (lock->adaptive_threshold > 50) ? lock->adaptive_threshold - 10 : 50;
    } else if (contention_ratio < 10) {
        // Low contention - allow more spinning
        lock->adaptive_threshold = (lock->adaptive_threshold < 200) ? lock->adaptive_threshold + 10 : 200;
    }
    // Medium contention (10-50%) - keep current threshold
}