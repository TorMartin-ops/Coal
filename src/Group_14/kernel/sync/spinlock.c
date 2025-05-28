#include <kernel/sync/spinlock.h>
#include <kernel/drivers/display/terminal.h> // For potential debug output

/**
 * @brief Initializes a spinlock to the unlocked state.
 */
void spinlock_init(spinlock_t *lock) {
    if (lock) {
        lock->locked = 0;
    }
}

/**
 * @brief Acquires the spinlock, disabling local interrupts first.
 * Uses GCC/Clang atomic built-ins for test-and-set.
 */
uintptr_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uintptr_t flags = local_irq_save(); // Disable interrupts, save state
    if (!lock) {
        terminal_write("[Spinlock] Error: Trying to acquire NULL lock!\n");
        // Optionally panic or handle error appropriately
        return flags; // Return saved flags even on error
    }

    // Atomically test-and-set: Set lock->locked to 1, return previous value.
    // Loop until the previous value was 0 (meaning we successfully acquired the lock).
    // __ATOMIC_ACQUIRE ensures memory operations after are not reordered before.
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // Spin (yield or pause instruction recommended for SMP)
        asm volatile ("pause" ::: "memory"); // Hint to CPU we are spinning
    }
    // Lock acquired

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