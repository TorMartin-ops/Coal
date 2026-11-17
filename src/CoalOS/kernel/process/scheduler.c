/**
 * O(1) scheduler. A bit per priority level says whether its FIFO queue is
 * non-empty; the next task is the head of the lowest set bit. All state is
 * touched with interrupts off (single CPU).
 */
#include <kernel/process/scheduler.h>
#include <kernel/process/process.h>
#include <kernel/process/exec.h>
#include <kernel/cpu/tss.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/memory/kmalloc.h>

_Static_assert(TIMER_HZ == 1000, "sleep_ms assumes a 1 kHz tick");

#define IDLE_STACK_SIZE 4096

struct switch_frame {            /* mirrors simple_switch in switch.asm */
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t ret;
};

volatile bool g_need_reschedule;

static tcb_t *rq_head[SCHED_PRIORITY_LEVELS];
static tcb_t *rq_tail[SCHED_PRIORITY_LEVELS];
static uint32_t rq_bitmap;
static tcb_t *sleep_head;                    /* sorted by wakeup_tick */
static tcb_t *volatile g_current;
static volatile uint32_t g_ticks;
static bool g_started;

static pcb_t idle_pcb;
#define IDLE (&idle_pcb.task)

uint32_t *scheduler_make_context(void *frame_top, void (*entry)(void), uint32_t eflags) {
    struct switch_frame *f = (struct switch_frame *)frame_top - 1;
    memset(f, 0, sizeof(*f));
    f->gs = f->fs = f->es = f->ds = 0x10;    /* kernel data selector */
    f->eflags = eflags;
    f->ret = (uint32_t)entry;
    return (uint32_t *)f;
}

static void rq_push(tcb_t *t) {
    uint8_t p = t->priority;
    t->state = TASK_READY;
    t->next = NULL;
    if (rq_tail[p]) rq_tail[p]->next = t; else rq_head[p] = t;
    rq_tail[p] = t;
    rq_bitmap |= 1u << p;
}

static tcb_t *rq_pop(void) {
    if (!rq_bitmap) return IDLE;
    uint32_t p = __builtin_ctz(rq_bitmap);
    tcb_t *t = rq_head[p];
    rq_head[p] = t->next;
    if (!rq_head[p]) {
        rq_tail[p] = NULL;
        rq_bitmap &= ~(1u << p);
    }
    t->next = NULL;
    return t;
}

static void switch_to(tcb_t *prev, tcb_t *next) {
    static uint32_t *boot_context;           /* sink for the boot stack */
    pcb_t *np = next->process;
    uint32_t cr3;

    g_current = next;
    tss_set_kernel_stack((uint32_t)np->kernel_stack_vaddr_top);
    if (np->page_directory_phys) {
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        if (cr3 != (uint32_t)np->page_directory_phys)
            asm volatile("mov %0, %%cr3" : : "r"(np->page_directory_phys) : "memory");
    }
    simple_switch(prev ? &prev->context : &boot_context, next->context);
}

void schedule(void) {
    uintptr_t flags = local_irq_save();
    tcb_t *prev = g_current;
    tcb_t *next;

    if (prev && prev->state == TASK_RUNNING) {
        if (prev == IDLE) prev->state = TASK_READY; else rq_push(prev);
    }
    next = rq_pop();
    next->state = TASK_RUNNING;
    next->slice = SCHED_SLICE_TICKS;
    if (next != prev) switch_to(prev, next);
    local_irq_restore(flags);

    signal_dispatch_current();
}

void yield(void) {
    schedule();
}

static void idle_loop(void) {
    for (;;) {
        process_reap_orphans();
        asm volatile("sti; hlt");
    }
}

void scheduler_init(void) {
    uint8_t *stack = kmalloc(IDLE_STACK_SIZE);

    if (!stack) KERNEL_PANIC_HALT("no memory for the idle stack");
    idle_pcb.task.process = &idle_pcb;
    idle_pcb.task.priority = SCHED_IDLE_PRIORITY;
    idle_pcb.kernel_stack_vaddr_top = (uint32_t *)(stack + IDLE_STACK_SIZE - 16);
    idle_pcb.task.context = scheduler_make_context(idle_pcb.kernel_stack_vaddr_top, idle_loop, 0x202);
    idle_pcb.task.state = TASK_READY;
}

void scheduler_start(void) {
    tcb_t *next;

    asm volatile("cli");                     /* the first task re-enables interrupts itself */
    next = rq_pop();
    g_started = true;
    next->state = TASK_RUNNING;
    next->slice = SCHED_SLICE_TICKS;
    switch_to(NULL, next);
    __builtin_unreachable();
}

int scheduler_add_task(pcb_t *proc) {
    uintptr_t flags = local_irq_save();
    proc->task.priority = SCHED_DEFAULT_PRIORITY;
    rq_push(&proc->task);
    local_irq_restore(flags);
    return 0;
}

void scheduler_unblock_task(tcb_t *task) {
    uintptr_t flags = local_irq_save();
    if (task->state == TASK_BLOCKED) {
        rq_push(task);
        if (g_current && task->priority < g_current->priority) g_need_reschedule = true;
    }
    local_irq_restore(flags);
}

void scheduler_wake(tcb_t *task) {
    uintptr_t flags = local_irq_save();
    if (task->state == TASK_SLEEPING) {
        tcb_t **pp = &sleep_head;
        while (*pp && *pp != task) pp = &(*pp)->next;
        if (*pp) *pp = task->next;
        rq_push(task);
    } else if (task->state == TASK_BLOCKED) {
        rq_push(task);
    }
    local_irq_restore(flags);
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) { yield(); return; }

    uintptr_t flags = local_irq_save();
    tcb_t *t = g_current, **pp = &sleep_head;

    t->wakeup_tick = g_ticks + ms;
    while (*pp && (int32_t)((*pp)->wakeup_tick - t->wakeup_tick) <= 0) pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->state = TASK_SLEEPING;
    schedule();
    local_irq_restore(flags);
}

void scheduler_exit_current(void) {
    asm volatile("cli");
    g_current->state = TASK_ZOMBIE;
    schedule();
    __builtin_unreachable();
}

void scheduler_tick(void) {
    tcb_t *cur = g_current;

    g_ticks++;
    while (sleep_head && (int32_t)(g_ticks - sleep_head->wakeup_tick) >= 0) {
        tcb_t *t = sleep_head;
        sleep_head = t->next;
        rq_push(t);
        if (cur && t->priority < cur->priority) g_need_reschedule = true;
    }
    if (!g_started || !cur) return;

    if (cur == IDLE) {
        if (rq_bitmap) schedule();
    } else if (cur->slice && --cur->slice == 0) {
        g_need_reschedule = false;
        schedule();
    } else if (g_need_reschedule) {
        g_need_reschedule = false;
        schedule();
    }
}

uint32_t scheduler_get_ticks(void) {
    return g_ticks;
}

tcb_t *get_current_task(void) {
    return g_current;
}

bool scheduler_is_ready(void) {
    return g_started;
}
