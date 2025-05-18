/**
 * @file scheduler.c
 * @brief UiAOS Priority-Based Preemptive Kernel Scheduler (Refactored & Fixed)
 * @author Tor Martin Kohle
 * @version 5.2
 *
 * @details Implements a priority-based preemptive scheduler.
 * Features multiple run queues, configurable time slices, sleep queue,
 * zombie task cleanup, TCB flag for run queue status, and a reschedule hint flag.
 * Assumes a timer interrupt calls scheduler_tick(). Fixes build errors from v5.1.
 */

//============================================================================
// Includes
//============================================================================
#include "scheduler.h"
#include "process.h"
#include "kmalloc.h"
#include "terminal.h"
#include "spinlock.h"
#include "idt.h"
#include "gdt.h"
#include "assert.h"
#include "paging.h"
#include "tss.h"
#include "serial.h"
#include "pit.h"
#include "port_io.h"
#include "keyboard_hw.h"
#include "keyboard.h" 
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <string.h>

//============================================================================
// Scheduler Configuration & Constants
//============================================================================
#define SCHED_PRIORITY_LEVELS   4
#define SCHED_DEFAULT_PRIORITY  1
#define SCHED_IDLE_PRIORITY     (SCHED_PRIORITY_LEVELS - 1)
#define SCHED_KERNEL_PRIORITY   0

#ifndef SCHED_TICKS_PER_SECOND
#define SCHED_TICKS_PER_SECOND  1000
#endif

#define MS_TO_TICKS(ms) (((ms) * SCHED_TICKS_PER_SECOND) / 1000)

static const uint32_t g_priority_time_slices_ms[SCHED_PRIORITY_LEVELS] = {
    200, /* P0 */ 100, /* P1 */ 50, /* P2 */ 25  /* P3 (Idle) */
};

// Error Codes
#define SCHED_OK          0
#define SCHED_ERR_NOMEM  (-1)
#define SCHED_ERR_FAIL   (-2)
#define SCHED_ERR_INVALID (-3)


// PHYS_TO_VIRT Macro (Same as before)
#ifndef KERNEL_PHYS_BASE
#define KERNEL_PHYS_BASE 0x100000u
#endif
#ifndef KERNEL_VIRT_BASE
#define KERNEL_VIRT_BASE 0xC0100000u
#endif
#define PHYS_TO_VIRT(p) ((uintptr_t)(p) >= KERNEL_PHYS_BASE ? \
                         ((uintptr_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE) : \
                         (uintptr_t)(p))

// Logging Macros (Corrected format specifiers)
#define SCHED_INFO(fmt, ...)  terminal_printf("[Sched INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) terminal_printf("[Sched DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) terminal_printf("[Sched ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  terminal_printf("[Sched WARN ] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_TRACE(fmt, ...) ((void)0)


#ifndef PIC1_DATA_PORT
#define PIC1_DATA_PORT  0x21 // Master PIC IMR port
#endif



//============================================================================
// Data Structures (Same as refactored v5.0)
//============================================================================
typedef struct {
    tcb_t      *head;
    tcb_t      *tail;
    uint32_t    count;
    spinlock_t  lock;
} run_queue_t;

typedef struct {
    tcb_t      *head;
    uint32_t    count;
    spinlock_t  lock;
} sleep_queue_t;

//============================================================================
// Module Static Data (Same as refactored v5.0)
//============================================================================
static run_queue_t   g_run_queues[SCHED_PRIORITY_LEVELS];
static sleep_queue_t g_sleep_queue;
static volatile tcb_t *g_current_task = NULL;
static tcb_t        *g_all_tasks_head = NULL;
static spinlock_t    g_all_tasks_lock;
static volatile uint32_t g_tick_count = 0;
static tcb_t         g_idle_task_tcb;
static pcb_t         g_idle_task_pcb;
volatile bool g_scheduler_ready = false;
volatile bool g_need_reschedule = false;

//============================================================================
// Forward Declarations (Assembly / Private Helpers) - Same as refactored v5.0
//============================================================================
extern void context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_pagedir);
extern void jump_to_user_mode(uint32_t *user_esp, uint32_t *pagedir);

static void init_run_queue(run_queue_t *queue);
static void init_sleep_queue(void);
static bool enqueue_task_locked(tcb_t *task);
static bool dequeue_task_locked(tcb_t *task);
static void add_to_sleep_queue_locked(tcb_t *task);
static void remove_from_sleep_queue_locked(tcb_t *task);
static void check_sleeping_tasks(void);
static tcb_t* scheduler_select_next_task(void);
static void perform_context_switch(tcb_t *old_task, tcb_t *new_task);
static void kernel_idle_task_loop(void) __attribute__((noreturn));
static void scheduler_init_idle_task(void);
void scheduler_cleanup_zombies(void);

//============================================================================
// Queue Management (Refined v5.0 implementations)
//============================================================================
static void init_run_queue(run_queue_t *queue) {
    KERNEL_ASSERT(queue != NULL, "NULL run queue pointer");
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    spinlock_init(&queue->lock);
}

static void init_sleep_queue(void) {
    g_sleep_queue.head = NULL;
    g_sleep_queue.count = 0;
    spinlock_init(&g_sleep_queue.lock);
}

static bool enqueue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot enqueue NULL task");
    KERNEL_ASSERT(task->state == TASK_READY, "Enqueueing task that is not READY");
    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority for enqueue");

    if (task->in_run_queue) {
        SCHED_WARN("Task PID %lu already marked as in_run_queue during enqueue attempt.", task->pid);
        return false;
    }

    run_queue_t *queue = &g_run_queues[task->priority];
    task->next = NULL;

    if (queue->tail) {
        queue->tail->next = task;
        queue->tail = task;
    } else {
        KERNEL_ASSERT(queue->head == NULL && queue->count == 0, "Queue tail is NULL but head isn't or count isn't 0");
        queue->head = task;
        queue->tail = task;
    }
    queue->count++;
    task->in_run_queue = true;
    return true;
}

static bool dequeue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot dequeue NULL task");
    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority for dequeue");

    run_queue_t *queue = &g_run_queues[task->priority];
    if (!queue->head) {
        SCHED_WARN("Attempted dequeue from empty queue Prio %u for task PID %lu", task->priority, task->pid);
        task->in_run_queue = false;
        return false;
    }

    if (queue->head == task) {
        queue->head = task->next;
        if (queue->tail == task) { queue->tail = NULL; KERNEL_ASSERT(queue->head == NULL, "Head non-NULL when tail dequeued");}
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow (head dequeue)");
        queue->count--;
        task->next = NULL;
        task->in_run_queue = false;
        return true;
    }

    tcb_t *prev = queue->head;
    while (prev->next && prev->next != task) {
        prev = prev->next;
    }

    if (prev->next == task) {
        prev->next = task->next;
        if (queue->tail == task) { queue->tail = prev; }
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow (mid/tail dequeue)");
        queue->count--;
        task->next = NULL;
        task->in_run_queue = false;
        return true;
    }

    SCHED_ERROR("Task PID %lu not found in Prio %u queue for dequeue!", task->pid, task->priority);
    task->in_run_queue = false; // Clear flag defensively
    return false;
}

static void add_to_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL && task->state == TASK_SLEEPING, "Invalid task/state for sleep queue add");
    KERNEL_ASSERT(!task->in_run_queue, "Sleeping task should not be marked as in_run_queue");

    task->wait_next = NULL;
    task->wait_prev = NULL;

    if (!g_sleep_queue.head || task->wakeup_time <= g_sleep_queue.head->wakeup_time) {
        task->wait_next = g_sleep_queue.head;
        if (g_sleep_queue.head) g_sleep_queue.head->wait_prev = task;
        g_sleep_queue.head = task;
    } else {
        tcb_t *current = g_sleep_queue.head;
        while (current->wait_next && current->wait_next->wakeup_time <= task->wakeup_time) {
            current = current->wait_next;
        }
        task->wait_next = current->wait_next;
        task->wait_prev = current;
        if (current->wait_next) current->wait_next->wait_prev = task;
        current->wait_next = task;
    }
    g_sleep_queue.count++;
}

static void remove_from_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot remove NULL from sleep queue");
    KERNEL_ASSERT(g_sleep_queue.count > 0, "Sleep queue count underflow");

    if (task->wait_prev) task->wait_prev->wait_next = task->wait_next;
    else g_sleep_queue.head = task->wait_next;

    if (task->wait_next) task->wait_next->wait_prev = task->wait_prev;

    task->wait_next = NULL;
    task->wait_prev = NULL;
    g_sleep_queue.count--;
}

static void check_sleeping_tasks(void) {
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    if (!g_sleep_queue.head) { spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags); return; }

    uint32_t current_ticks = g_tick_count;
    tcb_t *task = g_sleep_queue.head;
    bool task_woken = false;

    while (task && task->wakeup_time <= current_ticks) {
        tcb_t *task_to_wake = task;
        task = task->wait_next;
        remove_from_sleep_queue_locked(task_to_wake);
        task_to_wake->state = TASK_READY;
        spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags); // Release sleep lock

        SCHED_DEBUG("Waking up task PID %lu (Prio %u)", task_to_wake->pid, task_to_wake->priority);
        run_queue_t *queue = &g_run_queues[task_to_wake->priority];
        uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
        if (!enqueue_task_locked(task_to_wake)) {
             SCHED_ERROR("Failed to enqueue woken task PID %lu", task_to_wake->pid);
        }
        task_woken = true;
        spinlock_release_irqrestore(&queue->lock, queue_irq_flags); // Release queue lock

        sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock); // Re-acquire sleep lock
    }
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags); // Release final sleep lock
    if (task_woken) { g_need_reschedule = true; }
}

//============================================================================
// Tick Handler (Same as refactored v5.0)
//============================================================================
uint32_t scheduler_get_ticks(void) {
    return g_tick_count;
}

void scheduler_tick(void) {
    g_tick_count++;
    if (!g_scheduler_ready) return;

    check_sleeping_tasks();

    volatile tcb_t *curr_task_v = g_current_task;
    if (!curr_task_v) return;
    tcb_t *curr_task = (tcb_t *)curr_task_v;

    if (curr_task->pid == IDLE_TASK_PID) {
        if (g_need_reschedule) { g_need_reschedule = false; schedule(); }
        return;
    }

    curr_task->runtime_ticks++;
    if (curr_task->ticks_remaining > 0) {
        curr_task->ticks_remaining--;
    }

    if (curr_task->ticks_remaining == 0) {
        SCHED_DEBUG("Timeslice expired for PID %lu", curr_task->pid);
        g_need_reschedule = true;
    }

    if (g_need_reschedule) {
        g_need_reschedule = false;
        schedule();
    }
}

//============================================================================
// Idle Task & Zombie Cleanup (KBC Polling Removed)
//============================================================================
static __attribute__((noreturn)) void kernel_idle_task_loop(void) {
    SCHED_INFO("Idle task started (PID %lu). Entering HLT loop.", (unsigned long)IDLE_TASK_PID);
    // serial_write("[Idle Loop] Diagnostic checks will run before each HLT.\n"); // Already commented out in your log

    while (1) {
        scheduler_cleanup_zombies();

        // PATCH: Diagnostic block for idle loop (always on for this debugging phase,
        // or conditionally compiled with #ifdef VERBOSE_IDLE_DIAG if preferred later)
        // #define VERBOSE_IDLE_DIAG // Uncomment to make it conditional

// #ifdef VERBOSE_IDLE_DIAG // Or just keep it always on for now
        terminal_printf("[Idle Diagnostics] --- Pre-HLT Check ---\n");
        uint8_t kbc_status = inb(KBC_STATUS_PORT);
        terminal_printf("[Idle Diagnostics] KBC Status (Port 0x%x): 0x%x ", KBC_STATUS_PORT, kbc_status);
        terminal_printf("OBF=%d A2=%d | KB IRQs: %lu\n",
                         (kbc_status & KBC_SR_OBF) ? 1 : 0,
                         (kbc_status & KBC_SR_A2) ? 1 : 0,
                         (unsigned long)g_keyboard_irq_fire_count);

        uint8_t master_imr_before = inb(PIC1_DATA_PORT);
        terminal_printf("[Idle Diagnostics] PIC1 IMR (Port 0x%x) before: 0x%x. ", PIC1_DATA_PORT, master_imr_before);
        if (master_imr_before & 0x02) {
             terminal_write("IRQ1 MASKED!\n");
        } else {
            terminal_write("IRQ1 Unmasked (OK).\n");
        }

        // PIC ISR Check from review
        outb(PIC1_COMMAND, 0x0B); // OCW3 to read ISR
        uint8_t pic_isr = inb(PIC1_COMMAND); // Read ISR from master PIC
        terminal_printf("[Idle Diagnostics] PIC1 ISR: 0x%x ", pic_isr);
        if (pic_isr & 0x02) { terminal_write("(IRQ1 IN-SERVICE!)"); }
        terminal_write("\n");
        // Optional: Read IRR
        // outb(PIC1_COMMAND, 0x0A); // OCW3 to read IRR
        // uint8_t pic_irr = inb(PIC1_COMMAND);
        // terminal_printf("[Idle Diagnostics] PIC1 IRR: 0x%x\n", pic_irr);

        // terminal_printf("[Idle Diagnostics] Executing sti; hlt...\n"); // Can be a bit noisy
// #endif

        asm volatile ("sti; hlt");

        // serial_write("[Idle Loop] Woke up from hlt.\n"); // Can be very noisy
    }
}


static void scheduler_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");
    memset(&g_idle_task_pcb, 0, sizeof(pcb_t));
    g_idle_task_pcb.pid = IDLE_TASK_PID;
    g_idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    KERNEL_ASSERT(g_idle_task_pcb.page_directory_phys != NULL, "Kernel PD phys NULL during idle init");
    g_idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop;

    // --- Corrected Stack Setup ---
    static uint8_t idle_stack_buffer[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
    uintptr_t stack_buffer_phys_base = (uintptr_t)&idle_stack_buffer;
    uintptr_t stack_buffer_phys_top = stack_buffer_phys_base + sizeof(idle_stack_buffer);
    uintptr_t stack_top_virt_addr = PHYS_TO_VIRT(stack_buffer_phys_top);
    g_idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)stack_top_virt_addr;

    SCHED_DEBUG("Idle task stack: PhysBase=0x%lx, PhysTop=0x%lx -> VirtTop=0x%lx",
                  stack_buffer_phys_base, stack_buffer_phys_top, stack_top_virt_addr);

    memset(&g_idle_task_tcb, 0, sizeof(tcb_t));
    g_idle_task_tcb.process = &g_idle_task_pcb;
    g_idle_task_tcb.pid     = IDLE_TASK_PID;
    g_idle_task_tcb.state   = TASK_READY;
    g_idle_task_tcb.in_run_queue = false;
    g_idle_task_tcb.has_run = false;
    g_idle_task_tcb.priority = SCHED_IDLE_PRIORITY;
    KERNEL_ASSERT(g_idle_task_tcb.priority < SCHED_PRIORITY_LEVELS, "Idle priority out of bounds");
    g_idle_task_tcb.time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[g_idle_task_tcb.priority]);
    g_idle_task_tcb.ticks_remaining = g_idle_task_tcb.time_slice_ticks;

    uint32_t *kstack_ptr = (uint32_t*)stack_top_virt_addr;
    *(--kstack_ptr) = (uint32_t)g_idle_task_pcb.entry_point;
    *(--kstack_ptr) = 0; // Dummy EBP
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // GS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // FS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // ES
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // DS
    *(--kstack_ptr) = 0x00000202; // EFLAGS (IF=1)
    for (int i = 0; i < 8; i++) *(--kstack_ptr) = 0; // PUSHAD dummy
    g_idle_task_tcb.esp = kstack_ptr;
    SCHED_DEBUG("Idle task initial TCB ESP calculated: %p", g_idle_task_tcb.esp);

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    g_idle_task_tcb.all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = &g_idle_task_tcb;
    spinlock_release_irqrestore(&g_all_tasks_lock, irq_flags);
}

void scheduler_cleanup_zombies(void) {
    SCHED_TRACE("Checking for ZOMBIE tasks...");
    tcb_t *zombie_to_reap = NULL;
    tcb_t *prev_all = NULL;

    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    tcb_t *current_all = g_all_tasks_head;
    while (current_all) {
        if (current_all->pid != IDLE_TASK_PID && current_all->state == TASK_ZOMBIE) {
            zombie_to_reap = current_all;
            if (prev_all) prev_all->all_tasks_next = current_all->all_tasks_next;
            else g_all_tasks_head = current_all->all_tasks_next;
            zombie_to_reap->all_tasks_next = NULL;
            break;
        }
        prev_all = current_all;
        current_all = current_all->all_tasks_next;
    }
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);

    if (zombie_to_reap) {
        SCHED_INFO("Cleanup: Reaping ZOMBIE task PID %lu (Exit Code: %lu).", zombie_to_reap->pid, zombie_to_reap->exit_code);
        if (zombie_to_reap->process) destroy_process(zombie_to_reap->process);
        else SCHED_WARN("Zombie task PID %lu has NULL process pointer!", zombie_to_reap->pid);
        kfree(zombie_to_reap);
    }
}

//============================================================================
// Task Selection & Context Switching (Corrected format specifiers)
//============================================================================
static tcb_t* scheduler_select_next_task(void) {
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; ++prio) {
        run_queue_t *queue = &g_run_queues[prio];
        if (!queue->head) continue;
        uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
        tcb_t *task = queue->head;
        if (task) {
            bool dequeued = dequeue_task_locked(task);
            spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
            if (!dequeued) { SCHED_ERROR("Selected task PID %lu Prio %d but failed to dequeue!", task->pid, prio); continue; }
            task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[task->priority]);
            SCHED_DEBUG("Selected task PID %lu (Prio %d), Slice=%lu", task->pid, prio, task->ticks_remaining);
            return task;
        }
        spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    }
    g_idle_task_tcb.ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[g_idle_task_tcb.priority]);
    return &g_idle_task_tcb;
}

static void perform_context_switch(tcb_t *old_task, tcb_t *new_task) {
    KERNEL_ASSERT(new_task && new_task->process && new_task->esp && new_task->process->page_directory_phys, "Invalid new task");
    uintptr_t new_kernel_stack_top_vaddr = (new_task->pid == IDLE_TASK_PID)
        ? (uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top
        : (uintptr_t)new_task->process->kernel_stack_vaddr_top;
    tss_set_kernel_stack((uint32_t)new_kernel_stack_top_vaddr);
    bool pd_needs_switch = (!old_task || !old_task->process || old_task->process->page_directory_phys != new_task->process->page_directory_phys);

    if (!new_task->has_run && new_task->pid != IDLE_TASK_PID) {
        new_task->has_run = true;
        SCHED_DEBUG("First run for PID %lu. Jumping to user mode (ESP=%p, PD=%p)",
                      new_task->pid, new_task->esp, new_task->process->page_directory_phys);
        jump_to_user_mode(new_task->esp, new_task->process->page_directory_phys);
        KERNEL_PANIC_HALT("jump_to_user_mode returned!");
    } else {
        if (!new_task->has_run && new_task->pid == IDLE_TASK_PID) new_task->has_run = true;
        SCHED_DEBUG("Context switch: PID %lu (ESP=%p) -> PID %lu (ESP=%p) (PD Switch: %s)",
                      old_task ? old_task->pid : (uint32_t)-1, old_task ? old_task->esp : NULL,
                      new_task->pid, new_task->esp,
                      pd_needs_switch ? "YES" : "NO");
        context_switch(old_task ? &(old_task->esp) : NULL, new_task->esp,
                       pd_needs_switch ? new_task->process->page_directory_phys : NULL);
    }
}

void schedule(void) {
    if (!g_scheduler_ready) return;
    uint32_t eflags;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));

    tcb_t *old_task = (tcb_t *)g_current_task;
    tcb_t *new_task = scheduler_select_next_task();
    KERNEL_ASSERT(new_task != NULL, "scheduler_select_next_task returned NULL!");

    if (new_task == old_task) {
        if (old_task && old_task->state == TASK_READY) old_task->state = TASK_RUNNING;
        if (eflags & 0x200) asm volatile("sti");
        return;
    }

    if (old_task) {
        if (old_task->state == TASK_RUNNING) {
            old_task->state = TASK_READY;
            run_queue_t *queue = &g_run_queues[old_task->priority];
            uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
            if (!enqueue_task_locked(old_task)) {
                SCHED_ERROR("Failed to re-enqueue old task PID %lu", old_task->pid);
            }
            spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
        }
    }

    g_current_task = new_task;
    new_task->state = TASK_RUNNING;
    perform_context_switch(old_task, new_task);
}


//============================================================================
// Public API Functions (Corrected format specifiers)
//============================================================================
int scheduler_add_task(pcb_t *pcb) {
    KERNEL_ASSERT(pcb && pcb->pid != IDLE_TASK_PID && pcb->page_directory_phys &&
                  pcb->kernel_stack_vaddr_top && pcb->user_stack_top &&
                  pcb->entry_point && pcb->kernel_esp_for_switch, "Invalid PCB for add_task");

    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) { SCHED_ERROR("kmalloc TCB failed for PID %lu", pcb->pid); return SCHED_ERR_NOMEM; }
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb;
    new_task->pid     = pcb->pid;
    new_task->state   = TASK_READY;
    new_task->in_run_queue = false;
    new_task->has_run = false;
    new_task->esp     = (uint32_t*)pcb->kernel_esp_for_switch;
    new_task->priority = SCHED_DEFAULT_PRIORITY;
    KERNEL_ASSERT(new_task->priority < SCHED_PRIORITY_LEVELS, "Bad default prio");
    new_task->time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[new_task->priority]);
    new_task->ticks_remaining = new_task->time_slice_ticks;

    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    new_task->all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = new_task;
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);

    run_queue_t *queue = &g_run_queues[new_task->priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
    if (!enqueue_task_locked(new_task)) {
        SCHED_ERROR("Failed to enqueue newly created task PID %lu!", new_task->pid);
    }
    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);

    SCHED_INFO("Added task PID %lu (Prio %u, Slice %lu ticks)",
                 new_task->pid, new_task->priority, new_task->time_slice_ticks);
    return SCHED_OK;
}

void yield(void) {
    uint32_t eflags;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));
    SCHED_TRACE("yield() called by PID %lu", g_current_task ? g_current_task->pid : (uint32_t)-1);
    schedule();
    if (eflags & 0x200) asm volatile("sti");
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) { yield(); return; }
    uint32_t ticks_to_wait = MS_TO_TICKS(ms);
    if (ticks_to_wait == 0 && ms > 0) ticks_to_wait = 1;
    uint32_t current_ticks = scheduler_get_ticks();
    uint32_t wakeup_target;
    if (ticks_to_wait > (UINT32_MAX - current_ticks)) { wakeup_target = UINT32_MAX; SCHED_WARN("Sleep duration %lu ms results in tick overflow.", ms); }
    else { wakeup_target = current_ticks + ticks_to_wait; }

    asm volatile("cli");
    tcb_t *current = (tcb_t*)g_current_task;
    KERNEL_ASSERT(current && current->pid != IDLE_TASK_PID && (current->state == TASK_RUNNING || current->state == TASK_READY), "Invalid task state for sleep_ms");

    current->wakeup_time = wakeup_target;
    current->state = TASK_SLEEPING;
    current->in_run_queue = false;
    SCHED_DEBUG("Task PID %lu sleeping for %lu ms until tick %lu", current->pid, ms, current->wakeup_time);

    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    add_to_sleep_queue_locked(current);
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    schedule();
}

void remove_current_task_with_code(uint32_t code) {
    asm volatile("cli");
    tcb_t *task_to_terminate = (tcb_t *)g_current_task;
    KERNEL_ASSERT(task_to_terminate && task_to_terminate->pid != IDLE_TASK_PID, "Cannot terminate idle/null task");

    SCHED_INFO("Task PID %lu exiting with code %lu. Marking as ZOMBIE.", task_to_terminate->pid, code);
    task_to_terminate->state = TASK_ZOMBIE;
    task_to_terminate->exit_code = code;
    task_to_terminate->in_run_queue = false;
    schedule();
    KERNEL_PANIC_HALT("Returned from schedule() after terminating task!");
}

volatile tcb_t* get_current_task_volatile(void) { return g_current_task; }
tcb_t* get_current_task(void) { return (tcb_t *)g_current_task; }

void scheduler_start(void) {
    SCHED_INFO("Scheduler marked as ready.");
    g_scheduler_ready = true;
    g_need_reschedule = true;
}

void scheduler_init(void) {
    SCHED_INFO("Initializing scheduler (v5.2 - Final Format Fixes)...");
    memset(g_run_queues, 0, sizeof(g_run_queues));
    g_current_task = NULL;
    g_tick_count = 0;
    g_scheduler_ready = false;
    g_need_reschedule = false;
    g_all_tasks_head = NULL;
    spinlock_init(&g_all_tasks_lock);
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) init_run_queue(&g_run_queues[i]);
    init_sleep_queue();

    scheduler_init_idle_task();

    run_queue_t *idle_queue = &g_run_queues[g_idle_task_tcb.priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&idle_queue->lock);
    if (!enqueue_task_locked(&g_idle_task_tcb)) {
        KERNEL_PANIC_HALT("Failed to enqueue idle task");
    }
    spinlock_release_irqrestore(&idle_queue->lock, queue_irq_flags);

    g_current_task = &g_idle_task_tcb;

    uintptr_t idle_stack_top_virt = (uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top;
    SCHED_DEBUG("Setting initial TSS ESP0 to calculated high virtual top: %p", (void*)idle_stack_top_virt);
    tss_set_kernel_stack((uint32_t)idle_stack_top_virt);

    SCHED_INFO("Scheduler initialized (Idle Task PID %d ready).", IDLE_TASK_PID);
}

void scheduler_unblock_task(tcb_t *task) {
    if (!task) { SCHED_WARN("Called with NULL task."); return; }

    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority for unblock");
    run_queue_t *queue = &g_run_queues[task->priority];
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);

    if (task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        SCHED_DEBUG("Task PID %lu unblocked, new state: READY.", task->pid);
        if (!enqueue_task_locked(task)) {
             SCHED_ERROR("Failed to enqueue unblocked task PID %lu (already enqueued?)", task->pid);
        } else {
             g_need_reschedule = true;
             SCHED_DEBUG("Task PID %lu enqueued into run queue Prio %u.", task->pid, task->priority);
        }
    } else {
        SCHED_WARN("Called on task PID %lu which was not BLOCKED (state=%d).", task->pid, task->state);
    }

    spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
}