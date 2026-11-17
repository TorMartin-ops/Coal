/**
 * @file wait.h
 * @brief Minimal sleep/wakeup primitive for drivers (tty, serial).
 *
 * Usage, to avoid lost wakeups:
 *   flags = local_irq_save();
 *   while (!condition) drv_wait(&q);   // IRQs stay disabled across the check and the block
 *   local_irq_restore(flags);
 * and from the interrupt handler or any code with IRQs off: drv_wake_all(&q).
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

/** Makes every waiter runnable again. Safe from interrupt context. */
void drv_wake_all(drv_waitq_t *q);

#endif /* KERNEL_DRIVERS_WAIT_H */
