/**
 * @file mutex.h
 * @brief Sleeping lock for code that blocks (disk I/O) while holding it.
 *
 * Waiters sleep on a wait queue instead of spinning, so a preempted holder
 * costs them nothing. Not for interrupt context. Waiting is not interruptible.
 */
#ifndef KERNEL_SYNC_MUTEX_H
#define KERNEL_SYNC_MUTEX_H

#include <kernel/drivers/wait.h>

typedef struct {
    bool locked;
    drv_waitq_t waiters;
} mutex_t;

void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif
