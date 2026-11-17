/**
 * @file oom.c
 * @brief When a page fault finds no free frame, kill the process that uses
 *        the most memory (Linux does the same) instead of failing whichever
 *        process happens to ask next. One log line per kill.
 *
 * Runs only when memory is gone, so it may walk page tables: usage is the
 * number of present user pages.
 */
#define LOG_MODULE "oom"
#include <kernel/process/oom.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/memory/mm.h>
#include <kernel/fs/vfs/page_cache.h>
#include <kernel/core/log.h>
#include <kernel/drivers/wait.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/fs/vfs/fs_errno.h>

#define OOM_CACHE_PAGES 256

static drv_waitq_t g_released;

/* Killed or exiting: its memory is about to come back. */
static bool going_away(const pcb_t *p) { return p->exiting || p->oom_victim || (p->pending_signals & SIGBIT(SIGKILL)); }

static unsigned g_teardowns;                           /* address spaces being freed; touched with IRQs off */

void oom_teardown_begin(void) {
    uintptr_t irq = local_irq_save();
    g_teardowns++;
    local_irq_restore(irq);
}

void oom_teardown_end(void) {
    uintptr_t irq = local_irq_save();
    g_teardowns--;
    local_irq_restore(irq);
    drv_wake_all(&g_released);
}

/* Some process other than me is on its way out and still holds its address space. IRQs off. */
static bool victim_still_holds_memory(const pcb_t *me) {
    for (const pcb_t *p = process_all(); p; p = p->all_next)
        if (p != me && p->pid > 1 && p->mm && p->task.state != TASK_ZOMBIE && going_away(p)) return true;
    return false;
}

static size_t resident_pages(mm_struct_t *mm) {
    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    size_t n = 0;

    for (struct rb_node *r = rb_tree_first(&mm->vma_tree); r; r = rb_node_next(r)) {
        const vma_struct_t *v = rb_entry(r, vma_struct_t, rb_node);
        for (uintptr_t a = v->vm_start; a < v->vm_end; a += PAGE_SIZE) {
            pte_t *pte = paging_pte(mm->pgd_phys, a, false);
            n += pte && (*pte & PAGE_PRESENT);
        }
    }
    spinlock_release_irqrestore(&mm->lock, flags);
    return n;
}

bool oom_make_room(void) {
    pcb_t *me = get_current_process(), *victim = NULL;
    size_t most = 0;
    bool dying = false;

    if (pc_shrink(OOM_CACHE_PAGES)) return true;       /* cached file pages go before any process does */

    if (!me || going_away(me)) return false;           /* nothing to wait for if we are doomed */
    uintptr_t irq = local_irq_save();                   /* choose, mark and wait as one step: no lost wake-up, no second pick */
    for (pcb_t *p = process_all(); p; p = p->all_next) {
        if (p->pid <= 1 || !p->mm || p->task.state == TASK_ZOMBIE) continue;
        if (going_away(p)) { dying = true; continue; }
        size_t n = resident_pages(p->mm);
        if (n >= most) { most = n; victim = p; }
    }
    dying |= g_teardowns != 0;                          /* pages of a dead process are still on their way back */
    if (!dying && victim) {
        victim->oom_victim = true;
        if (victim == me) signal_force(SIGKILL);        /* ends it when it next returns to user mode */
        else process_kill((int)victim->pid, SIGKILL);
        LOG_WARN("out of memory: killing pid %u (%s), %u KiB resident", (unsigned)victim->pid, victim->name,
                 (unsigned)(most * (PAGE_SIZE / 1024)));
    }
    if (!dying && (!victim || victim == me)) {            /* nothing left to kill, or we are the biggest user */
        local_irq_restore(irq);
        return false;
    }
    uint64_t deadline = timer_uptime_ms() + OOM_WAIT_MS;
    while ((g_teardowns || victim_still_holds_memory(me)) && timer_uptime_ms() < deadline) {
        if (drv_wait_timeout(&g_released, OOM_WAIT_MS) == -EINTR) {   /* a signal is pending: do not spin on it */
            local_irq_restore(irq);
            asm volatile("sti");
            sleep_ms(10);
            irq = local_irq_save();
        }
    }
    local_irq_restore(irq);
    return true;
}
