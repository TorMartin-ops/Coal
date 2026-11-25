#include <kernel/sync/spinlock.h>
#ifdef COAL_DEBUG_KERNEL
#include <kernel/debug/lockdep.h>
#endif

#ifdef COAL_DEBUG_KERNEL
void spinlock_init_at(spinlock_t *lock, const char *site) {
    lock->locked = 0;
    lockdep_class_init(lock, &lock->cls, site);
}
#else
void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}
#endif

uintptr_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uintptr_t flags = local_irq_save();
#ifdef COAL_DEBUG_KERNEL
    lockdep_acquire(lock, &lock->cls, (uintptr_t)__builtin_return_address(0));
#endif
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
            asm volatile("pause" ::: "memory");
    }
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags) {
#ifdef COAL_DEBUG_KERNEL
    lockdep_release(lock, lock->cls);
#endif
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    local_irq_restore(flags);
}
