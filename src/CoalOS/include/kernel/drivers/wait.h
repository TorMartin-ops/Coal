/**
 * @file wait.h
 * @brief Minimal sleep/wakeup primitive for drivers and pipes.
 *
 * Usage, to avoid lost wakeups:
 *   flags = local_irq_save();
 *   while (!condition && !(rc = drv_wait(&q))) {}   // IRQs stay off across the check and the block
 *   local_irq_restore(flags);   // rc == -EINTR: a signal is pending
 * and from the interrupt handler or any other code: drv_wake_all(&q).
 */
#ifndef KERNEL_DRIVERS_WAIT_H
#define KERNEL_DRIVERS_WAIT_H

#include <kernel/process/scheduler.h>

typedef struct drv_waitq {
    tcb_t *head;
} drv_waitq_t;

/** Blocks the current task until drv_wake_all(). Caller must have IRQs disabled.
 *  Returns -EINTR at once, or when woken, if a signal is pending: the caller gives up
 *  and returns -EINTR itself. Before the scheduler runs it halts until the next interrupt. */
int drv_wait(drv_waitq_t *q);

/** Like drv_wait(), but returns after ms milliseconds at the latest. IRQs disabled. */
int drv_wait_timeout(drv_waitq_t *q, uint32_t ms);

/** Like drv_wait() for kernel-internal waits (mutexes): signals only cause a spurious return. */
void drv_wait_uninterruptible(drv_waitq_t *q);

/** Makes the waiters of q runnable (not the poll queue). Safe from interrupts. */
void drv_wake_queue(drv_waitq_t *q);

/** Takes a task off the queue it waits on (a signal woke it, or it is exiting). IRQs disabled. */
void drv_wait_cancel(tcb_t *t);

/** Makes every waiter runnable again, and every task blocked in poll(2). Safe from interrupts. */
void drv_wake_all(drv_waitq_t *q);

/** Queue of tasks waiting in poll(2); woken by every drv_wake_all(). */
extern drv_waitq_t drv_poll_queue;

#endif /* KERNEL_DRIVERS_WAIT_H */
