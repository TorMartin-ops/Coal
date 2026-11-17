#include <kernel/sync/spinlock.h>

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

uintptr_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uintptr_t flags = local_irq_save();
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
            asm volatile("pause" ::: "memory");
    }
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    local_irq_restore(flags);
}
