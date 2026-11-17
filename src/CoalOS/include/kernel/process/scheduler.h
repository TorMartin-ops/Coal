/**
 * @file scheduler.h
 * @brief O(1) priority scheduler: a bitmap of non-empty priority levels plus
 *        one FIFO run queue per level. Single CPU; the run queues are
 *        protected by disabling interrupts.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/core/types.h>

#define SCHED_PRIORITY_LEVELS  4   /* 0 = highest */
#define SCHED_DEFAULT_PRIORITY 1
#define SCHED_IDLE_PRIORITY    3
#define SCHED_SLICE_TICKS      20

struct pcb;

typedef enum {
    TASK_READY,     /* on a run queue */
    TASK_RUNNING,
    TASK_BLOCKED,   /* waiting for scheduler_unblock_task() */
    TASK_SLEEPING,  /* on the sleep list */
    TASK_ZOMBIE     /* exited, waiting to be reaped */
} task_state_t;

typedef struct tcb {
    struct tcb  *next;          /* run queue / sleep list link */
    struct pcb  *process;
    uint32_t    *context;       /* saved kernel ESP (see switch.asm) */
    task_state_t state;
    uint8_t      priority;
    uint32_t     slice;         /* ticks left in the current slice */
    uint32_t     wakeup_tick;
    struct tcb  *wait_next;     /* driver wait queue link (drivers/wait.h) */
    struct drv_waitq *wait_q;   /* the queue the task sits on, NULL when none */
} tcb_t;

/** Creates the idle task. Call once, before any task is added. */
void scheduler_init(void);

/** Switches to the first task and never returns. */
void scheduler_start(void) __attribute__((noreturn));

/** Makes a new task runnable. Returns 0. */
int scheduler_add_task(struct pcb *proc);

/** Picks the next task and switches to it. Callable from tasks and from the tick ISR. */
void schedule(void);

/** Gives up the CPU but stays runnable. */
void yield(void);

/** Sleeps for at least ms milliseconds (timer-tick based, no busy wait). */
void sleep_ms(uint32_t ms);

/**
 * Wakes a task that set its state to TASK_BLOCKED. Safe if the task has not
 * called schedule() yet and for tasks that are not blocked (no-op).
 */
void scheduler_unblock_task(tcb_t *task);

/** Wakes a blocked or sleeping task so it can notice a pending signal. */
void scheduler_wake(tcb_t *task);

/** Marks the current task a zombie and switches away for good. */
void scheduler_exit_current(void) __attribute__((noreturn));

/** Timer interrupt hook: counts ticks, wakes sleepers, enforces time slices. */
void scheduler_tick(void);

tcb_t *get_current_task(void);

/** True once scheduler_start() has switched to the first task. */
bool scheduler_is_ready(void);

extern volatile bool g_need_reschedule;   /* polled by syscall.asm on return to user mode */

/**
 * Lays out a first-run switch frame just below frame_top so that switching to
 * the returned context "returns" into entry with the given EFLAGS.
 */
uint32_t *scheduler_make_context(void *frame_top, void (*entry)(void), uint32_t eflags);

/* switch.asm */
void simple_switch(uint32_t **save_esp, uint32_t *new_esp);
void ret_from_fork(void);

#endif
