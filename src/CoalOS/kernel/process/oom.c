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
#include <kernel/core/log.h>

#define WAIT_MS 10

/* Killed or exiting: its memory is about to come back. */
static bool going_away(const pcb_t *p) { return p->exiting || (p->pending_signals & (1u << SIGKILL)); }

static size_t resident_pages(mm_struct_t *mm) {
    uintptr_t flags = spinlock_acquire_irqsave(&mm->lock);
    size_t n = 0;

    for (struct rb_node *r = rb_tree_first(&mm->vma_tree); r; r = rb_node_next(r)) {
        const vma_struct_t *v = rb_entry(r, vma_struct_t, rb_node);
        for (uintptr_t a = v->vm_start; a < v->vm_end; a += PAGE_SIZE) {
            uint32_t *pte = paging_pte(mm->pgd_phys, a, false);
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

    if (!me || going_away(me)) return false;           /* nothing to wait for if we are doomed */
    uintptr_t irq = local_irq_save();                   /* choose and kill as one step, once for all faulting tasks */
    for (pcb_t *p = process_all(); p; p = p->all_next) {
        if (p->pid <= 1 || !p->mm || p->task.state == TASK_ZOMBIE) continue;
        if (going_away(p)) { dying = true; continue; }
        size_t n = resident_pages(p->mm);
        if (n >= most) { most = n; victim = p; }
    }
    if (!dying && victim) {
        if (victim == me) signal_force(SIGKILL);        /* ends it when it next returns to user mode */
        else process_kill((int)victim->pid, SIGKILL);
        LOG_WARN("out of memory: killing pid %u (%s), %u KiB resident", (unsigned)victim->pid, victim->name,
                 (unsigned)(most * (PAGE_SIZE / 1024)));
    }
    local_irq_restore(irq);
    if (victim == me || (!dying && !victim)) return false;

    asm volatile("sti");                                /* a page fault arrives with interrupts off */
    sleep_ms(WAIT_MS);
    return true;
}
