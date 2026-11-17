#include <kernel/drivers/wait.h>

drv_waitq_t drv_poll_queue;

static void enqueue(drv_waitq_t *q, tcb_t *t) {
    t->wait_next = q->head;
    t->wait_q = q;
    q->head = t;
}

static void dequeue(drv_waitq_t *q, tcb_t *t) {
    for (tcb_t **pp = &q->head; *pp; pp = &(*pp)->wait_next)
        if (*pp == t) { *pp = t->wait_next; break; }
    t->wait_next = NULL;
    t->wait_q = NULL;
}

void drv_wait_cancel(tcb_t *t) {
    if (t->wait_q) dequeue(t->wait_q, t);
}

void drv_wait(drv_waitq_t *q) {
    tcb_t *self = scheduler_is_ready() ? get_current_task() : NULL;
    if (!self) {
        asm volatile("sti; hlt; cli" ::: "memory");
        return;
    }
    enqueue(q, self);
    self->state = TASK_BLOCKED;
    schedule();
    drv_wait_cancel(self);               /* woken by a signal rather than by drv_wake_all() */
}

void drv_wait_timeout(drv_waitq_t *q, uint32_t ms) {
    tcb_t *self = get_current_task();

    enqueue(q, self);
    sleep_ms(ms);                        /* a wakeup from the queue ends it early */
    dequeue(q, self);                    /* still queued if the timer fired */
}

static void wake_queue(drv_waitq_t *q) {
    tcb_t *t = q->head;
    q->head = NULL;
    while (t) {
        tcb_t *next = t->wait_next;
        t->wait_next = NULL;
        t->wait_q = NULL;
        if (t->state == TASK_BLOCKED) scheduler_unblock_task(t);
        else if (t->state == TASK_SLEEPING) scheduler_wake(t);
        t = next;
    }
}

void drv_wake_all(drv_waitq_t *q) {
    wake_queue(q);
    if (q != &drv_poll_queue) wake_queue(&drv_poll_queue);
}
