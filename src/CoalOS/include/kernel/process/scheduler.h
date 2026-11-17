/**
 * @file scheduler.h
 * @brief EEVDF scheduler (Earliest Eligible Virtual Deadline First). Single CPU; all state is
 *        protected by disabling interrupts. See scheduler.c for the algorithm.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/core/types.h>
#include <kernel/lib/rbtree.h>
#include <kernel/drivers/timer/timer.h>

#define NICE_MIN (-20)
#define NICE_MAX 19

struct pcb;

typedef enum {
    TASK_READY,     /* on a run queue */
    TASK_RUNNING,
    TASK_BLOCKED,   /* waiting for scheduler_unblock_task() */
    TASK_SLEEPING,  /* waiting for its sleep timer */
    TASK_ZOMBIE     /* exited, waiting to be reaped */
} task_state_t;

enum { RQ_NONE, RQ_READY, RQ_PENDING };

typedef struct tcb {
    struct rb_node rb;          /* node of the run queue tree named by `queue` */
    struct pcb  *process;
    uint64_t    *context;       /* saved kernel RSP (see switch.asm) */
    task_state_t state;
    int8_t       nice;
    uint8_t      queue;         /* RQ_* */
    bool         protect;       /* keeps the CPU until its virtual deadline passes */
    uint32_t     weight, inv_weight;    /* Linux prio_to_weight, and 2^32 / weight */
    uint32_t     slice_ns;      /* requested time slice: short for tasks that sleep a lot */
    uint32_t     ran_ns;        /* CPU time since the task last got the CPU */
    uint64_t     vruntime;      /* virtual time received: CPU time scaled by 1024 / weight */
    uint64_t     deadline;      /* virtual time by which the current slice should be done */
    int64_t      vlag;          /* virtual lag kept while the task is not runnable */
    uint64_t     exec_start;    /* clock_ns() when the task last started or was accounted */
    struct ktimer sleep_timer;
    struct tcb  *wait_next;     /* driver wait queue link (drivers/wait.h) */
    struct drv_waitq *wait_q;   /* the queue the task sits on, NULL when none */
} tcb_t;

/** Creates the idle task. Call once, before any task is added. */
void scheduler_init(void);

/** Switches to the first task and never returns. */
void scheduler_start(void) __attribute__((noreturn));

/** Makes a new task runnable (it inherits proc->task.nice). Returns 0. */
int scheduler_add_task(struct pcb *proc);

/** Picks the next task and switches to it. Callable from tasks and from interrupt handlers. */
void schedule(void);

/** Gives up the CPU but stays runnable. */
void yield(void);

/** Sleeps for at least ms milliseconds (no busy wait). */
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

/** Clock event hook (timer.c): wheel timers have run; enforces time slices. May switch tasks. */
void scheduler_timer_event(uint64_t now_ns);

/** Sets the nice value (clamped to NICE_MIN..NICE_MAX); the CPU share follows the Linux weight table. */
void scheduler_set_nice(tcb_t *task, int nice);

tcb_t *get_current_task(void);

/** True once scheduler_start() has switched to the first task. */
bool scheduler_is_ready(void);

extern volatile bool g_need_reschedule;   /* polled by syscall.asm and the interrupt exit on return to user mode */

/**
 * Lays out a first-run switch frame just below frame_top so that switching to
 * the returned context "returns" into entry with the given RFLAGS.
 */
uint64_t *scheduler_make_context(void *frame_top, void (*entry)(void), uint64_t rflags);

/* switch.asm, isr_stubs.asm */
void simple_switch(uint64_t **save_rsp, uint64_t *new_rsp);
void ret_from_fork(void);

#endif
