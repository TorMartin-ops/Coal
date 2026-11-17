/**
 * EEVDF scheduler: Earliest Eligible Virtual Deadline First (Stoica & Abdel-Wahab, "Earliest
 * Eligible Virtual Deadline First", TR-95-22, 1995; the variant in Linux 6.6, Zijlstra 2023).
 *
 * Every runnable task has a weight (Linux nice table), a virtual runtime v (CPU time scaled by
 * 1024/weight) and a virtual deadline d = v + slice/weight. V is the weighted average of all v; a
 * task is eligible when v <= V, i.e. it has not received more than its fair share. The next task is
 * the eligible one with the earliest deadline. A task's lag V - v is kept while it sleeps, so
 * sleeping neither earns nor loses CPU share.
 *
 * Two red-black trees hold the queued tasks: `ready` (eligible, ordered by deadline) and `pending`
 * (not yet eligible, ordered by v). Picking moves tasks whose time has come from `pending` to
 * `ready`, then takes the leftmost of `ready`: O(log n). The running task is not in a tree but is
 * part of the sums, and keeps the CPU until its deadline passes (run to parity), which bounds the
 * number of context switches. Tasks that sleep after a short burst ask for a short slice, so their
 * deadlines come early and they are picked quickly after a wake-up.
 *
 * There is no periodic tick: the clock event device is armed for the end of the running task's
 * slice, and only while another task is waiting for the CPU.
 */
#include <kernel/process/scheduler.h>
#include <kernel/process/process.h>
#include <kernel/process/rlimit.h>
#include <kernel/process/fpu.h>
#include <kernel/process/exec.h>
#include <kernel/cpu/tss.h>
#include <kernel/cpu/gdt.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/lib/div64.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/frame.h>

#define IDLE_STACK_SIZE 4096
#define NICE_0_WEIGHT   1024
#define SLICE_NORMAL_NS 3000000u    /* CPU-bound tasks */
#define SLICE_SHORT_NS   750000u    /* tasks that sleep after less than this; Linux 6.6's base slice */
#define LAG_FLOOR_NS    1000000u
#define REBASE_NS       (1u << 28)  /* keep the offsets small enough for 64-bit sums */

/* Linux kernel/sched/core.c, prio_to_weight: each nice step is about 10% CPU. */
static const uint32_t prio_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

struct switch_frame {            /* mirrors simple_switch in switch.asm */
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t ret;
};

volatile bool g_need_reschedule;

static struct {
    struct rb_tree ready, pending;
    uint32_t nr;                 /* tasks in the two trees */
    uint32_t load;               /* sum of the weights of all runnable tasks, the running one included */
    int64_t  lag_sum;            /* sum of weight * (v - origin) over the same tasks */
    uint64_t origin;             /* reference point for the offsets; moved when they grow */
} rq = { .origin = 1ull << 62 };   /* far from 0, so v - lag never wraps */

static tcb_t *volatile g_current;
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

/* ---- virtual time ------------------------------------------------------------------- */

static int64_t offset(const tcb_t *t) {
    return (int64_t)(t->vruntime - rq.origin);
}

/* V, the weighted average of v. The quotient is the mean offset, which rebase() keeps far below 2^31,
 * so one IDIV does it; the remainder fixes the rounding toward zero into a floor. */
static uint64_t avg_vruntime(void) {
    int32_t q, r;

    if (!rq.load) return rq.origin;
    asm("idivl %4" : "=a"(q), "=d"(r) : "0"((uint32_t)rq.lag_sum), "1"((int32_t)(rq.lag_sum >> 32)), "r"(rq.load));
    return rq.origin + (uint64_t)(int64_t)(r < 0 ? q - 1 : q);
}

static bool eligible(const tcb_t *t) {
    return rq.lag_sum >= offset(t) * (int64_t)rq.load;      /* v <= V without dividing */
}

/* Real time to virtual time: ns * 1024 / weight. */
static uint64_t to_virtual(uint64_t ns, const tcb_t *t) {
    return t->weight == NICE_0_WEIGHT ? ns : mul64_32_shr32(ns << 10, t->inv_weight);
}

static uint64_t slice_virtual(const tcb_t *t) {
    return to_virtual(t->slice_ns, t);
}

/* Real time the task can still run before its deadline. */
static uint64_t slice_left_ns(const tcb_t *t) {
    return t->deadline > t->vruntime ? ((t->deadline - t->vruntime) * t->weight) >> 10 : 0;
}

static void set_weight(tcb_t *t, int nice) {
    t->nice = (int8_t)(nice < NICE_MIN ? NICE_MIN : nice > NICE_MAX ? NICE_MAX : nice);
    t->weight = prio_to_weight[t->nice - NICE_MIN];
    t->inv_weight = (uint32_t)div64_32(1ull << 32, t->weight);
}

static void sums_add(const tcb_t *t) {
    rq.load += t->weight;
    rq.lag_sum += (int64_t)t->weight * offset(t);
}

static void sums_sub(const tcb_t *t) {
    rq.load -= t->weight;
    rq.lag_sum -= (int64_t)t->weight * offset(t);
}

static void rebase(void) {
    uint64_t v = avg_vruntime();

    rq.lag_sum -= (int64_t)(v - rq.origin) * (int64_t)rq.load;
    rq.origin = v;
}

/* Charges the CPU time since the last call to t. True if its slice (virtual deadline) is used up;
 * it then gets a new deadline and loses its protection. */
static bool update_curr(tcb_t *t, uint64_t now) {
    uint64_t delta, dv;

    if (t == IDLE || (int64_t)(now - t->exec_start) <= 0) return false;   /* `now` may predate a charge made by a wake-up */
    delta = now - t->exec_start;
    t->exec_start = now;
    rlimit_charge(t->process, delta);
    t->ran_ns = delta > 0xFFFFFFFFu - t->ran_ns ? 0xFFFFFFFFu : t->ran_ns + (uint32_t)delta;
    dv = to_virtual(delta, t);
    t->vruntime += dv;
    rq.lag_sum += (int64_t)t->weight * (int64_t)dv;
    if (offset(t) > REBASE_NS) rebase();
    if (t->vruntime < t->deadline) return false;
    t->deadline = t->vruntime + slice_virtual(t);
    t->protect = false;
    return true;
}

/* ---- run queue ---------------------------------------------------------------------- */

static bool queued(void) {
    return rq.nr != 0;
}

static void tree_insert(struct rb_tree *tree, tcb_t *t, bool by_deadline) {
    struct rb_node *parent = NULL, *n = tree->root;
    uint64_t key = by_deadline ? t->deadline : t->vruntime;
    bool left = true;

    while (n) {
        const tcb_t *c = rb_entry(n, tcb_t, rb);

        parent = n;
        left = (int64_t)(key - (by_deadline ? c->deadline : c->vruntime)) < 0;
        n = left ? n->left : n->right;
    }
    rb_tree_insert_at(tree, parent, &t->rb, left);
    rq.nr++;
}

static void queue_add(tcb_t *t) {
    if (eligible(t)) {
        t->queue = RQ_READY;
        tree_insert(&rq.ready, t, true);
    } else {
        t->queue = RQ_PENDING;
        tree_insert(&rq.pending, t, false);
    }
}

static void queue_del(tcb_t *t) {
    rb_tree_remove(t->queue == RQ_READY ? &rq.ready : &rq.pending, &t->rb);
    t->queue = RQ_NONE;
    rq.nr--;
}

static tcb_t *first(struct rb_tree *tree) {
    struct rb_node *n = rb_tree_first(tree);

    return n ? rb_entry(n, tcb_t, rb) : NULL;
}

/* The task to run next: the eligible one with the earliest deadline. `curr` is the running task if it
 * is still runnable (it is not in a tree). Returns NULL if nothing is runnable. */
static tcb_t *pick(tcb_t *curr) {
    tcb_t *t;

    if (!curr && rq.nr == 1) return first(rq.ready.root ? &rq.ready : &rq.pending);   /* nothing to choose */
    if (curr && !eligible(curr)) curr = NULL;
    if (curr && curr->protect) return curr;
    while ((t = first(&rq.pending)) && eligible(t)) {       /* their time has come */
        queue_del(t);
        t->queue = RQ_READY;
        tree_insert(&rq.ready, t, true);
    }
    while ((t = first(&rq.ready)) && !eligible(t)) {        /* V moved back since they were queued */
        queue_del(t);
        t->queue = RQ_PENDING;
        tree_insert(&rq.pending, t, false);
    }
    t = first(&rq.ready);
    if (curr && (!t || curr->deadline <= t->deadline)) return curr;
    return t ? t : first(&rq.pending);
}

/* Puts a task that starts running or wakes up on the queue, at its remembered lag. */
static void place(tcb_t *t, bool is_new) {
    uint64_t vslice = slice_virtual(t);

    t->vruntime = avg_vruntime() - (is_new ? 0 : (uint64_t)t->vlag);
    t->deadline = t->vruntime + (is_new ? vslice / 2 : vslice);
    t->ran_ns = 0;
    sums_add(t);
}

/* Takes a task that stops being runnable out of the sums; a sleeper keeps its lag. */
static void unplace(tcb_t *t) {
    if (t->state != TASK_ZOMBIE) {
        int64_t lag = (int64_t)(avg_vruntime() - t->vruntime);
        int64_t limit = (int64_t)to_virtual(2 * t->slice_ns > LAG_FLOOR_NS ? 2 * t->slice_ns : LAG_FLOOR_NS, t);

        t->vlag = lag < -limit ? -limit : lag > limit ? limit : lag;
        t->slice_ns = t->ran_ns < SLICE_SHORT_NS ? SLICE_SHORT_NS : SLICE_NORMAL_NS;
    }
    sums_sub(t);
}

/* ---- switching ---------------------------------------------------------------------- */

static void switch_to(tcb_t *prev, tcb_t *next) {
    static uint32_t *boot_context;           /* sink for the boot stack */
    pcb_t *np = next->process;
    uint32_t cr3;

    g_current = next;
    if (!prev || np->tls_base != prev->process->tls_base || np->tls_pages != prev->process->tls_pages)
        gdt_set_tls_base(np->tls_base, np->tls_pages);
    tss_set_kernel_stack((uint32_t)np->kernel_stack_vaddr_top);
    fpu_task_switch(np);
    if (np->page_directory_phys) {
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        if (cr3 != (uint32_t)np->page_directory_phys)
            asm volatile("mov %0, %%cr3" : : "r"(np->page_directory_phys) : "memory");
    }
    simple_switch(prev ? &prev->context : &boot_context, next->context);
}

/* Makes `next` the running task: protects its slice and, if someone waits, arms the slice timer. */
static void start_running(tcb_t *next, uint64_t now) {
    next->state = TASK_RUNNING;
    if (next == IDLE) return;
    next->protect = true;
    if (queued()) clockevent_request(now + slice_left_ns(next));
    if (next->process->cpu_next != RLIMIT_CPU_NEVER) clockevent_request(now + rlimit_cpu_left_ns(next->process));
}

void schedule(void) {
    uintptr_t flags = local_irq_save();
    tcb_t *prev = g_current, *next;
    uint64_t now = clock_ns();

    g_need_reschedule = false;
    if (prev != IDLE) update_curr(prev, now);
    if (prev == IDLE) {
        next = pick(NULL);
    } else if (prev->state == TASK_RUNNING) {
        next = queued() ? pick(prev) : prev;
        if (next != prev) {
            prev->state = TASK_READY;
            queue_add(prev);
        }
    } else {
        unplace(prev);
        next = pick(NULL);
    }
    if (!next) next = IDLE;
    else if (next->queue != RQ_NONE) queue_del(next);

    start_running(next, now);
    if (next != prev) {
        next->exec_start = now;
        switch_to(prev, next);
    }
    local_irq_restore(flags);
}

void yield(void) {
    tcb_t *t = g_current;

    if (g_started && t != IDLE && queued()) {   /* go behind the others: give up the rest of the slice */
        uintptr_t flags = local_irq_save();
        t->deadline += slice_virtual(t);
        t->protect = false;
        local_irq_restore(flags);
    }
    schedule();
}

static void idle_loop(void) {
    for (;;) {
        process_reap_orphans();
        if (frame_refill_zeroed()) {            /* idle time pays for the pool of zeroed frames */
            if (g_need_reschedule) schedule();
            continue;
        }
        asm volatile("cli");
        if (g_need_reschedule) {
            asm volatile("sti");
            schedule();
        } else {
            asm volatile("sti; hlt");
        }
    }
}

void scheduler_init(void) {
    uint8_t *stack = kmalloc(IDLE_STACK_SIZE);

    if (!stack) KERNEL_PANIC_HALT("no memory for the idle stack");
    idle_pcb.task.process = &idle_pcb;
    idle_pcb.kernel_stack_vaddr_top = (uint32_t *)(stack + IDLE_STACK_SIZE - 16);
    idle_pcb.task.context = scheduler_make_context(idle_pcb.kernel_stack_vaddr_top, idle_loop, 0x202);
    idle_pcb.task.state = TASK_READY;
}

void scheduler_start(void) {
    tcb_t *next;
    uint64_t now = clock_ns();

    asm volatile("cli");                     /* the first task re-enables interrupts itself */
    next = pick(NULL);
    if (!next) next = IDLE;
    else queue_del(next);
    g_started = true;
    next->exec_start = now;
    start_running(next, now);
    switch_to(NULL, next);
    __builtin_unreachable();
}

/* ---- wake-ups ----------------------------------------------------------------------- */

/* A task has become runnable: the running one keeps the CPU until its slice ends, so only make sure
 * the slice timer is armed (or, if the slice is already over, reschedule now). */
static void check_preempt(void) {
    tcb_t *cur = g_current;

    if (!g_started) return;
    if (cur == IDLE || (cur->state == TASK_RUNNING && !cur->protect)) {
        g_need_reschedule = true;
    } else if (cur->state == TASK_RUNNING) {
        clockevent_request(cur->exec_start + slice_left_ns(cur));
    }
}

static void make_runnable(tcb_t *t) {
    if (t == g_current) {                    /* still on the CPU, only about to block: cancel that */
        t->state = TASK_RUNNING;
        return;
    }
    place(t, false);
    t->state = TASK_READY;
    queue_add(t);
    check_preempt();
}

static void sleep_expired(struct ktimer *timer) {
    tcb_t *t = container_of(timer, tcb_t, sleep_timer);

    if (t->state == TASK_SLEEPING) make_runnable(t);
}

int scheduler_add_task(pcb_t *proc) {
    tcb_t *t = &proc->task;
    uintptr_t flags = local_irq_save();

    set_weight(t, t->nice);
    t->slice_ns = SLICE_NORMAL_NS;
    t->sleep_timer.fn = sleep_expired;
    place(t, true);
    t->state = TASK_READY;
    queue_add(t);
    check_preempt();
    local_irq_restore(flags);
    return 0;
}

void scheduler_unblock_task(tcb_t *task) {
    uintptr_t flags = local_irq_save();

    if (task->state == TASK_BLOCKED) make_runnable(task);
    local_irq_restore(flags);
}

void scheduler_wake(tcb_t *task) {
    uintptr_t flags = local_irq_save();

    if (task->state == TASK_SLEEPING) {
        timer_del(&task->sleep_timer);
        make_runnable(task);
    } else if (task->state == TASK_BLOCKED) {
        make_runnable(task);
    }
    local_irq_restore(flags);
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) { yield(); return; }

    uintptr_t flags = local_irq_save();
    tcb_t *t = g_current;

    timer_add(&t->sleep_timer, ms);
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

void scheduler_timer_event(uint64_t now) {
    tcb_t *cur = g_current;

    if (!g_started) return;
    if (cur != IDLE) {
        bool used_up = update_curr(cur, now);

        if (used_up && queued()) g_need_reschedule = true;
        else if (queued()) clockevent_request(now + slice_left_ns(cur));
    }
    if (g_need_reschedule) schedule();
}

void scheduler_set_nice(tcb_t *t, int nice) {
    uintptr_t flags = local_irq_save();
    uint32_t old = t->weight;
    bool runnable = t->state == TASK_READY || t->state == TASK_RUNNING;

    if (runnable && t != IDLE) {
        bool was_queued = t->queue != RQ_NONE;
        uint64_t left;

        if (t == g_current) update_curr(t, clock_ns());      /* charge the time so far at the old weight */
        left = t->deadline - t->vruntime;                    /* the unused slice keeps its real length */
        if (was_queued) queue_del(t);
        sums_sub(t);
        set_weight(t, nice);
        sums_add(t);
        t->deadline = t->vruntime + div64_32(left * old, t->weight);
        if (was_queued) queue_add(t);
    } else {
        set_weight(t, nice);
        t->vlag = div64_s32_floor(t->vlag * (int64_t)old, t->weight);   /* the same real lag */
    }
    local_irq_restore(flags);
}

tcb_t *get_current_task(void) {
    return g_current;
}

bool scheduler_is_ready(void) {
    return g_started;
}
