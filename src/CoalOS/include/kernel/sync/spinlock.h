/**
 * @file spinlock.h
 * @brief Test-and-set spinlock with IRQ save/restore (single CPU today, SMP-ready)
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <kernel/core/types.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

void spinlock_init(spinlock_t *lock);

/** Disables interrupts, takes the lock, returns the previous EFLAGS. */
uintptr_t spinlock_acquire_irqsave(spinlock_t *lock);

/** Drops the lock and restores the EFLAGS returned by acquire. */
void spinlock_release_irqrestore(spinlock_t *lock, uintptr_t flags);

static inline uintptr_t local_irq_save(void) {
    uintptr_t flags;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void local_irq_restore(uintptr_t flags) {
    asm volatile("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

#endif
