#include <kernel/drivers/wait.h>

void drv_wait(drv_waitq_t *q) {
    tcb_t *self = scheduler_is_ready() ? get_current_task() : NULL;
    if (!self) {
        asm volatile("sti; hlt; cli" ::: "memory");
        return;
    }
    self->wait_next = q->head;
    q->head = self;
    self->state = TASK_BLOCKED;
    schedule();
}

void drv_wake_all(drv_waitq_t *q) {
    tcb_t *t = q->head;
    q->head = NULL;
    while (t) {
        tcb_t *next = t->wait_next;
        t->wait_next = NULL;
        if (t->state == TASK_BLOCKED) {
            scheduler_unblock_task(t);
        }
        t = next;
    }
}
