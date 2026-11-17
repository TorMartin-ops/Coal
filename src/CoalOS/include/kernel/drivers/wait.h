/**
 * @file wait.h
 * @brief Minimal sleep/wakeup primitive for drivers and pipes.
 *
 * Usage, to avoid lost wakeups:
 *   flags = local_irq_save();
 *   while (!condition) drv_wait(&q);   // IRQs stay disabled across the check and the block
 *   local_irq_restore(flags);
 * and from the interrupt handler or any other code: drv_wake_all(&q).
 */
#ifndef KERNEL_DRIVERS_WAIT_H
#define KERNEL_DRIVERS_WAIT_H

#include <kernel/process/scheduler.h>

typedef struct drv_waitq {
    tcb_t *head;
} drv_waitq_t;

/** Blocks the current task until drv_wake_all(). Caller must have IRQs disabled.
 *  Before the scheduler runs it halts until the next interrupt instead. */
void drv_wait(drv_waitq_t *q);

/** Like drv_wait(), but returns after ms milliseconds at the latest. IRQs disabled. */
void drv_wait_timeout(drv_waitq_t *q, uint32_t ms);

/** Makes every waiter runnable again, and every task blocked in poll(2). Safe from interrupts. */
void drv_wake_all(drv_waitq_t *q);

/** Queue of tasks waiting in poll(2); woken by every drv_wake_all(). */
extern drv_waitq_t drv_poll_queue;

#endif /* KERNEL_DRIVERS_WAIT_H */
