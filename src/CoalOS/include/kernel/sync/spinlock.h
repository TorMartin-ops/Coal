/**
 * @file spinlock.h
 * @brief Test-and-set spinlock with IRQ save/restore (single CPU today, SMP-ready)
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <kernel/core/types.h>

typedef struct {
    volatile uint32_t locked;
#ifdef COAL_DEBUG_KERNEL
    uint16_t cls;                       /* lock class for the lock checker (debug kernel) */
#endif
} spinlock_t;

#ifdef COAL_DEBUG_KERNEL
/* The place that initialises a lock names its class for the lock checker. */
void spinlock_init_at(spinlock_t *lock, const char *site);
#define SPINLOCK_STR2(x) #x
#define SPINLOCK_STR(x)  SPINLOCK_STR2(x)
#define spinlock_init(l) spinlock_init_at((l), __FILE__ ":" SPINLOCK_STR(__LINE__))
#else
void spinlock_init(spinlock_t *lock);
#endif

/** Disables interrupts, takes the lock, returns the previous RFLAGS. */
uintptr_t spinlock_acquire_irqsave(spinlock_t *lock);

/** Drops the lock and restores the RFLAGS returned by acquire. */
void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags);

static inline uintptr_t local_irq_save(void) {
    uintptr_t flags;
    asm volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void local_irq_restore(uintptr_t flags) {
    asm volatile("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

#endif
