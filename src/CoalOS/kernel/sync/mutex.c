#include <kernel/sync/mutex.h>
#include <kernel/sync/spinlock.h>

void mutex_lock(mutex_t *m) {
    uintptr_t flags = local_irq_save();
    while (m->locked) drv_wait_uninterruptible(&m->waiters);
    m->locked = true;
    local_irq_restore(flags);
}

void mutex_unlock(mutex_t *m) {
    uintptr_t flags = local_irq_save();
    m->locked = false;
    drv_wake_queue(&m->waiters);
    local_irq_restore(flags);
}
