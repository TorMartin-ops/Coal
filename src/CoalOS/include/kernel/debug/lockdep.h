/**
 * @file lockdep.h
 * @brief Lock checker of the debug kernel.
 *
 * Spinlocks belong to a class: the place that initialises them, or for a static lock that is never
 * initialised, the lock itself. Every acquisition while other locks are held records "held -> acquired";
 * a cycle among the classes is a possible deadlock and is reported once. Also reported: taking a lock the
 * CPU already holds (certain deadlock), a release of a lock that is not held, and blocking (schedule, mutex,
 * a user copy that may fault) while a spinlock is held or inside an interrupt handler.
 */
#ifndef KERNEL_DEBUG_LOCKDEP_H
#define KERNEL_DEBUG_LOCKDEP_H

#include <kernel/core/types.h>

struct spinlock;

#ifdef COAL_DEBUG_KERNEL

#define LOCKDEP_MAX_HELD 16

/** What the checker knows about the running task: interrupt nesting and the spinlocks it holds. It travels with the
 *  task, because a handler can switch away (preemption) and the task that resumes was in no handler. */
struct lockdep_state {
    uint16_t irq_depth, nheld;
    struct { void *lock; uint16_t cls; uintptr_t ip; } held[LOCKDEP_MAX_HELD];
};

/** Context switch: save the running task's state in prev, load next's. */
void lockdep_switch(struct lockdep_state *prev, const struct lockdep_state *next);

void lockdep_class_init(void *lock, uint16_t *cls, const char *site);
void lockdep_acquire(void *lock, uint16_t *cls, uintptr_t ip);
void lockdep_release(void *lock, uint16_t cls);

/** Interrupt handlers bracket themselves with these. */
void lockdep_irq_enter(void);
void lockdep_irq_exit(void);

/** The caller is about to block (what names it) or to leave the kernel: no spinlock may be held. */
void lockdep_might_sleep(const char *what);
void lockdep_schedule(void);                    /* schedule() may run in a handler, but never under a spinlock */
void lockdep_no_locks(const char *what);

#define might_sleep() lockdep_might_sleep(__func__)

#else

#define might_sleep() ((void)0)
static inline void lockdep_schedule(void) {}
static inline void lockdep_irq_enter(void) {}
static inline void lockdep_irq_exit(void) {}
static inline void lockdep_no_locks(const char *what) {}

#endif
#endif
