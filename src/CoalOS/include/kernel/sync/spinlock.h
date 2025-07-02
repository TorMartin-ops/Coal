#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <kernel/core/types.h> // For uintptr_t, bool

/**
 * @brief Enhanced spinlock structure with performance optimizations.
 * Uses a volatile integer as the lock flag.
 * 0 = unlocked, 1 = locked.
 */
typedef struct {
    volatile uint32_t locked; // Use uint32_t for atomic operations
    // Performance optimization fields
    volatile uint32_t contention_count; // Number of times lock was contended
    volatile uint32_t total_acquisitions; // Total number of lock acquisitions
    uint32_t adaptive_threshold; // Threshold for adaptive spinning
    // Optional: Debugging info (owner CPU, etc.) could be added here.
} spinlock_t;

/**
 * @brief Initializes a spinlock to the unlocked state.
 *
 * @param lock Pointer to the spinlock_t structure.
 */
void spinlock_init(spinlock_t *lock);

/**
 * @brief Acquires the spinlock, disabling local interrupts.
 *
 * Spins (busy-waits) until the lock is acquired. Disables interrupts on the
 * current CPU before attempting to acquire the lock and returns an opaque
 * value representing the previous interrupt state.
 *
 * @param lock Pointer to the spinlock_t structure.
 * @return An opaque value representing the previous interrupt state (to be used with restore).
 */
uintptr_t spinlock_acquire_irqsave(spinlock_t *lock);

/**
 * @brief Releases the spinlock and restores the previous interrupt state.
 *
 * @param lock Pointer to the spinlock_t structure.
 * @param flags The opaque value returned by spinlock_acquire_irqsave.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags);

/**
 * @brief Helper function to disable local interrupts and return previous flags.
 * IMPLEMENTATION DEPENDENT (likely requires inline assembly).
 */
static inline uintptr_t local_irq_save(void) {
    uintptr_t flags;
    asm volatile (
        "pushfl\n\t"        // Push EFLAGS
        "pop %0\n\t"        // Pop into flags variable
        "cli"               // Disable interrupts
        : "=r" (flags)      // Output: flags
        :                   // No input
        : "memory"          // Clobbers memory (due to stack operation)
    );
    return flags;
}

/**
 * @brief Helper function to restore local interrupt state from flags.
 * IMPLEMENTATION DEPENDENT (requires inline assembly).
 */
static inline void local_irq_restore(uintptr_t flags) {
    asm volatile (
        "push %0\n\t"       // Push flags value
        "popfl"             // Pop into EFLAGS
        :                   // No output
        : "r" (flags)       // Input: flags
        : "memory", "cc"    // Clobbers memory and condition codes
    );
}

//============================================================================
// Spinlock Performance Optimizations
//============================================================================

/**
 * @brief Tries to acquire spinlock without spinning (non-blocking).
 * 
 * @param lock Pointer to the spinlock_t structure.
 * @return Previous interrupt flags if acquired, 0 if lock was already held.
 */
uintptr_t spinlock_try_acquire_irqsave(spinlock_t *lock);

/**
 * @brief Get spinlock performance statistics.
 * 
 * @param lock Pointer to the spinlock_t structure.
 * @param contention_count Output: number of times lock was contended.
 * @param total_acquisitions Output: total number of acquisitions.
 */
void spinlock_get_stats(spinlock_t *lock, uint32_t *contention_count, uint32_t *total_acquisitions);

/**
 * @brief Reset spinlock performance statistics.
 * 
 * @param lock Pointer to the spinlock_t structure.
 */
void spinlock_reset_stats(spinlock_t *lock);

#endif // SPINLOCK_H