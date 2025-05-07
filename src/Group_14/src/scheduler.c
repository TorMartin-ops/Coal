/**
 * @file scheduler.c
 * @brief UiAOS Priority-Based Preemptive Kernel Scheduler
 * @author Tor Martin Kohle
 * @version 4
 *
 * @details Implements a priority-based preemptive scheduler.
 * Features multiple run queues, configurable time slices, sleep queue,
 * and zombie task cleanup. Assumes a timer interrupt calls scheduler_tick().
 */

//============================================================================
// Includes
//============================================================================
#include "scheduler.h"
#include "process.h"
#include "kmalloc.h"
#include "terminal.h" // For terminal_printf (used by SCHED_INFO, etc.)
#include "spinlock.h"
#include "idt.h"
#include "gdt.h"
#include "assert.h"
#include "paging.h"
#include "tss.h"
#include "serial.h"   // For direct serial_write in critical/early paths if needed
#include "pit.h"      // For scheduler_get_ticks() if PIT provides it
#include "port_io.h"  // For inb/outb (used in diagnostics)

// Keyboard hardware definitions for diagnostics (ideally from a dedicated kbc.h)
#ifndef KBC_STATUS_PORT
#define KBC_STATUS_PORT 0x64
#endif
#ifndef KBC_DATA_PORT
#define KBC_DATA_PORT   0x60
#endif
#ifndef KBC_SR_OUT_BUF
#define KBC_SR_OUT_BUF  0x01 // Bit 0: Output buffer full
#endif
#ifndef PIC1_DATA_PORT
#define PIC1_DATA_PORT  0x21 // Master PIC IMR port
#endif

#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <string.h>      // memset

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


#ifndef KERNEL_PHYS_BASE // These are defined in the macro itself as fallbacks
#define KERNEL_PHYS_BASE 0x100000u
#endif
#ifndef KERNEL_VIRT_BASE // These are defined in the macro itself as fallbacks
#define KERNEL_VIRT_BASE 0xC0100000u
#endif
#define PHYS_TO_VIRT(p) ((uintptr_t)(p) >= KERNEL_PHYS_BASE ? \
                         ((uintptr_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE) : \
                         (uintptr_t)(p))

// Debug Logging Macros (Reduced verbosity for general operation)
#define SCHED_INFO(fmt, ...)  terminal_printf("[Sched INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) terminal_printf("[Sched DEBUG] " fmt "\n", ##__VA_ARGS__) // Keep for key debug points
#define SCHED_ERROR(fmt, ...) terminal_printf("[Sched ERROR] " fmt "\n", ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  terminal_printf("[Sched WARN ] " fmt "\n", ##__VA_ARGS__) // <<< ADD THIS DEFINITION
#define SCHED_TRACE(fmt, ...) ((void)0) // Disabled by default for cleaner logs

//============================================================================
// Data Structures
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
// Module Static Data
//============================================================================
static run_queue_t   g_run_queues[SCHED_PRIORITY_LEVELS];
static sleep_queue_t g_sleep_queue;
static volatile tcb_t *g_current_task = NULL;
static uint32_t      g_task_count = 0;
static spinlock_t    g_scheduler_global_lock;
static volatile uint32_t g_tick_count = 0; // Managed by scheduler_tick
static tcb_t         g_idle_task_tcb;
static pcb_t         g_idle_task_pcb;
static tcb_t        *g_all_tasks_head = NULL;
static spinlock_t    g_all_tasks_lock;
volatile bool g_scheduler_ready = false;

//============================================================================
// Forward Declarations
//============================================================================
static tcb_t* scheduler_select_next_task(void);
static void   perform_context_switch(tcb_t *old_task, tcb_t *new_task);
extern void   context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_pagedir);
extern void   jump_to_user_mode(uint32_t *user_esp, uint32_t *pagedir);

static void   init_run_queue(run_queue_t *queue);
static void   enqueue_task_locked(tcb_t *task);
static bool   dequeue_task_locked(tcb_t *task);
static void   add_to_sleep_queue_locked(tcb_t *task);
static void   remove_from_sleep_queue_locked(tcb_t *task);
static void   check_sleeping_tasks_locked(void);
static void   kernel_idle_task_loop(void) __attribute__((noreturn));
static void   scheduler_init_idle_task(void);
void scheduler_cleanup_zombies(void); // Declaration for use in idle_task_loop

//============================================================================
// Queue Management (Implementations mostly unchanged, ensure they are robust)
//============================================================================
static void init_run_queue(run_queue_t *queue) {
    KERNEL_ASSERT(queue != NULL, "NULL queue");
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

static void enqueue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL && task->priority < SCHED_PRIORITY_LEVELS, "Bad task/prio");
    run_queue_t *queue = &g_run_queues[task->priority];
    task->next = NULL;
    if (queue->tail) {
        queue->tail->next = task;
    } else {
        queue->head = task;
    }
    queue->tail = task;
    queue->count++;
}

static bool dequeue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL && task->priority < SCHED_PRIORITY_LEVELS, "Bad task/prio");
    run_queue_t *queue = &g_run_queues[task->priority];
    if (!queue->head) return false;

    if (queue->head == task) {
        queue->head = task->next;
        if (queue->tail == task) queue->tail = NULL;
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow");
        queue->count--;
        task->next = NULL;
        return true;
    }

    tcb_t *prev = queue->head;
    while (prev->next && prev->next != task) {
        prev = prev->next;
    }
    if (prev->next == task) {
        prev->next = task->next;
        if (queue->tail == task) queue->tail = prev;
        KERNEL_ASSERT(queue->count > 0, "Queue count underflow");
        queue->count--;
        task->next = NULL;
        return true;
    }
    SCHED_ERROR("Task PID %lu not found in Prio %u queue for dequeue!", task->pid, task->priority);
    return false;
}

static void add_to_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL && task->state == TASK_SLEEPING, "Bad task state for sleep");
    task->wait_next = NULL; task->wait_prev = NULL;

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
    KERNEL_ASSERT(task != NULL, "Cannot remove NULL from sleep");
    if (task->wait_prev) task->wait_prev->wait_next = task->wait_next;
    else if (g_sleep_queue.head == task) g_sleep_queue.head = task->wait_next;
    if (task->wait_next) task->wait_next->wait_prev = task->wait_prev;
    task->wait_next = NULL; task->wait_prev = NULL;
    KERNEL_ASSERT(g_sleep_queue.count > 0, "Sleep queue count underflow");
    g_sleep_queue.count--;
}

static void check_sleeping_tasks_locked(void) {
    if (!g_sleep_queue.head) return;
    uint32_t current_ticks = g_tick_count;
    tcb_t *task = g_sleep_queue.head;
    while (task && task->wakeup_time <= current_ticks) {
        tcb_t *task_to_wake = task;
        task = task->wait_next;
        remove_from_sleep_queue_locked(task_to_wake);
        task_to_wake->state = TASK_READY;
        uintptr_t qflags = spinlock_acquire_irqsave(&g_run_queues[task_to_wake->priority].lock);
        enqueue_task_locked(task_to_wake);
        spinlock_release_irqrestore(&g_run_queues[task_to_wake->priority].lock, qflags);
        SCHED_DEBUG("Woke up task PID %lu", task_to_wake->pid);
    }
}

//============================================================================
// Tick Handler
//============================================================================
uint32_t scheduler_get_ticks(void) {
    return g_tick_count;
}

void scheduler_tick(void) {
    g_tick_count++;
    if (!g_scheduler_ready) return;

    // Reduce verbose logging from scheduler_tick for normal operation
    // SCHED_TRACE("Tick: %lu", g_tick_count);

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    check_sleeping_tasks_locked();
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    volatile tcb_t *curr_task_v = g_current_task;
    if (!curr_task_v) return;
    tcb_t *curr_task = (tcb_t*)curr_task_v;

    if (curr_task->pid == IDLE_TASK_PID) {
        schedule(); // Check if other tasks are ready
        return;
    }

    curr_task->runtime_ticks++;
    if (curr_task->ticks_remaining > 0) {
        curr_task->ticks_remaining--;
    }

    if (curr_task->ticks_remaining == 0) {
        SCHED_DEBUG("Timeslice expired for PID %lu", curr_task->pid);
        schedule();
    }
}

//============================================================================
// Idle Task & Zombie Cleanup
//============================================================================
static __attribute__((noreturn)) void kernel_idle_task_loop(void) {
    // This log is essential for confirming idle task startup
    SCHED_INFO("Idle task started (PID %lu). Entering HLT loop.", (unsigned long)IDLE_TASK_PID);
    
    // Print this once before entering the loop for clarity
    serial_write("[Idle Loop] Diagnostic checks will run before each HLT.\n");

    while (1) {
        // Cleanup zombies first
        scheduler_cleanup_zombies();

        // --- BEGIN DIAGNOSTICS for Keyboard Issue ---
        terminal_printf("[Idle Diagnostics] --- Pre-HLT Check ---\n");

        // 1. KBC Status
        uint8_t kbc_status = inb(KBC_STATUS_PORT);
        terminal_printf("[Idle Diagnostics] KBC Status: 0x%x ", kbc_status);
        if (kbc_status & KBC_SR_OUT_BUF) {
            uint8_t kbc_data = inb(KBC_DATA_PORT);
            terminal_printf("(OBF, Data: 0x%x - Cleared)\n", kbc_data);
        } else {
            terminal_printf("(OBE)\n");
        }
        
        // 2. PIC1 IMR Status & Forced Unmask for IRQ1
        uint8_t master_imr_before = inb(PIC1_DATA_PORT);
        terminal_printf("[Idle Diagnostics] PIC1 IMR before: 0x%x. ", master_imr_before);
        if (master_imr_before & 0x02) { // Bit 1 for IRQ1
            terminal_write("IRQ1 MASKED! Forcing unmask... ");
        }
        outb(PIC1_DATA_PORT, master_imr_before & ~0x02); // Ensure IRQ1 is unmasked
        uint8_t master_imr_after = inb(PIC1_DATA_PORT);
        terminal_printf("PIC1 IMR after: 0x%x\n", master_imr_after);
        
        terminal_printf("[Idle Diagnostics] Executing sti; hlt...\n");
        // --- END DIAGNOSTICS ---

        asm volatile ("sti; hlt");
        
        // This log is CRITICAL. If it doesn't appear after a key press,
        // the keyboard IRQ didn't wake the CPU or wasn't processed up to here.
        // It *should* appear for PIT ticks.
        serial_write("[Idle Loop] Woke up from hlt.\n");
    }
}

static void scheduler_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");
    memset(&g_idle_task_pcb, 0, sizeof(pcb_t));
    g_idle_task_pcb.pid = IDLE_TASK_PID;
    g_idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    KERNEL_ASSERT(g_idle_task_pcb.page_directory_phys != NULL, "Kernel PD NULL");
    g_idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop;
    
    static uint8_t idle_stack_buffer[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
    KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 512, "Idle stack too small");
    g_idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)((uintptr_t)idle_stack_buffer + sizeof(idle_stack_buffer));
    
    memset(&g_idle_task_tcb, 0, sizeof(tcb_t));
    g_idle_task_tcb.process = &g_idle_task_pcb;
    g_idle_task_tcb.pid     = IDLE_TASK_PID;
    g_idle_task_tcb.state   = TASK_READY;
    g_idle_task_tcb.has_run = true; // Considered 'run' as it's the base state
    g_idle_task_tcb.priority = SCHED_IDLE_PRIORITY;
    g_idle_task_tcb.time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[SCHED_IDLE_PRIORITY]);
    g_idle_task_tcb.ticks_remaining = g_idle_task_tcb.time_slice_ticks;

    // Prepare initial kernel stack for the idle task to start in kernel_idle_task_loop
    uint32_t *kstack_ptr = (uint32_t*)PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);
    *(--kstack_ptr) = (uint32_t)g_idle_task_pcb.entry_point; // Return address for context_switch's ret
    *(--kstack_ptr) = 0; // Fake EBP
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // DS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // ES
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // FS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // GS
    *(--kstack_ptr) = 0x00000202; // EFLAGS (IF=1)
    for (int i = 0; i < 8; i++) *(--kstack_ptr) = 0; // EDI, ESI, EBP(from popad), ESP_dummy, EBX, EDX, ECX, EAX
    g_idle_task_tcb.esp = kstack_ptr;

    SCHED_DEBUG("Idle task TCB ESP: %p", g_idle_task_tcb.esp);

    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    g_idle_task_tcb.all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = &g_idle_task_tcb;
    spinlock_release_irqrestore(&g_all_tasks_lock, irq_flags);
}

void scheduler_cleanup_zombies(void) {
    SCHED_TRACE("Checking for ZOMBIE tasks...");
    tcb_t *zombie_to_reap = NULL;
    tcb_t *prev_all = NULL;
    bool lock_held = false; // Track if all_tasks_lock is held

    // Find one zombie task
    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    lock_held = true;

    tcb_t *current_all = g_all_tasks_head;
    while (current_all) {
        if (current_all->pid != IDLE_TASK_PID && current_all->state == TASK_ZOMBIE) {
            zombie_to_reap = current_all;
            if (prev_all) {
                prev_all->all_tasks_next = current_all->all_tasks_next;
            } else {
                g_all_tasks_head = current_all->all_tasks_next;
            }
            zombie_to_reap->all_tasks_next = NULL; // Isolate the zombie

            // Decrement global task count (under global scheduler lock)
            uintptr_t sched_lock_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
            KERNEL_ASSERT(g_task_count > 0, "Task count underflow on zombie reap");
            g_task_count--;
            spinlock_release_irqrestore(&g_scheduler_global_lock, sched_lock_flags);
            break; // Found one, break to release lock and reap
        }
        prev_all = current_all;
        current_all = current_all->all_tasks_next;
    }
    
    if (lock_held) {
        spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);
    }

    // Reap outside the lock
    if (zombie_to_reap) {
        SCHED_INFO("Cleanup: Reaping ZOMBIE task PID %lu.", zombie_to_reap->pid);
        if (zombie_to_reap->process) {
            destroy_process(zombie_to_reap->process); // Frees PCB and its resources
        }
        kfree(zombie_to_reap); // Free TCB
    }
}

//============================================================================
// Task Selection & Context Switching
//============================================================================
static tcb_t* scheduler_select_next_task(void) {
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; ++prio) {
        run_queue_t *queue = &g_run_queues[prio];
        if (!queue->head) continue;

        uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
        tcb_t *task = queue->head;
        if (task) {
            bool dequeued = dequeue_task_locked(task); // Assumes this function is correct
            spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
            if (!dequeued) {
                 SCHED_ERROR("Dequeue failed for PID %lu Prio %d!", task->pid, prio);
                 continue;
            }
            task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[task->priority]);
            SCHED_DEBUG("Selected task PID %lu (Prio %d)", task->pid, prio);
            return task;
        }
        spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
    }
    g_idle_task_tcb.ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[g_idle_task_tcb.priority]);
    return &g_idle_task_tcb; // Fallback to idle task
}

static void perform_context_switch(tcb_t *old_task, tcb_t *new_task) {
    KERNEL_ASSERT(new_task && new_task->process && new_task->esp && new_task->process->page_directory_phys, "Invalid new task");
    
    uintptr_t new_kernel_stack_top;
    if (new_task->pid != IDLE_TASK_PID) {
        new_kernel_stack_top = (uintptr_t)new_task->process->kernel_stack_vaddr_top;
    } else {
        new_kernel_stack_top = PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);
    }
    tss_set_kernel_stack((uint32_t)new_kernel_stack_top);

    bool pd_needs_switch = (!old_task || !old_task->process || old_task->process->page_directory_phys != new_task->process->page_directory_phys);

    if (!new_task->has_run && new_task->pid != IDLE_TASK_PID) {
        new_task->has_run = true;
        SCHED_DEBUG("First run for PID %lu. Jumping to user mode (ESP=%p, PD=%p)",
                      new_task->pid, new_task->esp, new_task->process->page_directory_phys);
        jump_to_user_mode(new_task->esp, new_task->process->page_directory_phys);
        // Does not return
    } else {
        if (!new_task->has_run && new_task->pid == IDLE_TASK_PID) new_task->has_run = true;
        SCHED_DEBUG("Context switch: %lu -> %lu (PD Switch: %s)",
                      old_task ? old_task->pid : (uint32_t)-1, new_task->pid, pd_needs_switch ? "YES" : "NO");
        context_switch(old_task ? &(old_task->esp) : NULL, new_task->esp,
                       pd_needs_switch ? new_task->process->page_directory_phys : NULL);
    }
}

void schedule(void) {
    if (!g_scheduler_ready) return;
    uint32_t eflags_val; // Renamed to avoid conflict with eflags in process.h context
    asm volatile("pushf; pop %0; cli" : "=r"(eflags_val)); // Save and disable interrupts

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    check_sleeping_tasks_locked();
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);

    tcb_t *old_task = (tcb_t *)g_current_task;
    tcb_t *new_task = scheduler_select_next_task();

    if (new_task == old_task) {
        if (old_task && old_task->state == TASK_READY) old_task->state = TASK_RUNNING;
        spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
        if (eflags_val & 0x200) asm volatile("sti"); // Restore IF if it was set
        return;
    }

    if (old_task) {
        if (old_task->state == TASK_RUNNING) {
            old_task->state = TASK_READY;
            uintptr_t old_q_flags = spinlock_acquire_irqsave(&g_run_queues[old_task->priority].lock);
            enqueue_task_locked(old_task);
            spinlock_release_irqrestore(&g_run_queues[old_task->priority].lock, old_q_flags);
        }
    }

    new_task->state = TASK_RUNNING;
    g_current_task = new_task;
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
    
    perform_context_switch(old_task, new_task);
    // Interrupts restored by context_switch (iret)
}

//============================================================================
// Public API Functions
//============================================================================
int scheduler_add_task(pcb_t *pcb) {
    KERNEL_ASSERT(pcb && pcb->pid != IDLE_TASK_PID && pcb->page_directory_phys &&
                  pcb->kernel_stack_vaddr_top && pcb->user_stack_top &&
                  pcb->entry_point && pcb->kernel_esp_for_switch, "Invalid PCB for add_task");

    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        SCHED_ERROR("kmalloc TCB failed for PID %lu", pcb->pid);
        return SCHED_ERR_NOMEM;
    }
    memset(new_task, 0, sizeof(tcb_t));
    new_task->process = pcb;
    new_task->pid     = pcb->pid;
    new_task->state   = TASK_READY;
    new_task->esp     = (uint32_t*)pcb->kernel_esp_for_switch;
    new_task->priority = SCHED_DEFAULT_PRIORITY;
    KERNEL_ASSERT(new_task->priority < SCHED_PRIORITY_LEVELS, "Bad default prio");
    new_task->time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[new_task->priority]);
    new_task->ticks_remaining = new_task->time_slice_ticks;

    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    new_task->all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = new_task;
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[new_task->priority].lock);
    enqueue_task_locked(new_task);
    g_task_count++;
    spinlock_release_irqrestore(&g_run_queues[new_task->priority].lock, queue_irq_flags);
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    SCHED_INFO("Added task PID %lu (Prio %u, Slice %lu ticks). Total active: %lu",
                 new_task->pid, new_task->priority, new_task->time_slice_ticks, g_task_count);
    return SCHED_OK;
}

void yield(void) {
    asm volatile("cli"); // Disable interrupts before calling schedule
    schedule();
    // Interrupts restored by context_switch or if schedule returns without switching
    asm volatile("sti"); // Ensure interrupts are re-enabled if no switch occurred
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) { yield(); return; }

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    tcb_t *current = (tcb_t*)g_current_task;
    KERNEL_ASSERT(current && current->pid != IDLE_TASK_PID &&
                 (current->state == TASK_RUNNING || current->state == TASK_READY),
                  "Invalid task state for sleep_ms");

    uint32_t ticks_to_wait = MS_TO_TICKS(ms);
    if (ticks_to_wait == 0 && ms > 0) ticks_to_wait = 1;
    if (ticks_to_wait > (UINT32_MAX / 2)) ticks_to_wait = UINT32_MAX / 2; // Prevent huge waits
    
    current->wakeup_time = g_tick_count + ticks_to_wait;
    current->state = TASK_SLEEPING;
    SCHED_DEBUG("Task PID %lu sleeping for %lu ms until tick %lu", current->pid, ms, current->wakeup_time);

    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    add_to_sleep_queue_locked(current);
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    asm volatile("cli");
    schedule(); // Switch away from sleeping task
    // Interrupts restored by context_switch
}

void remove_current_task_with_code(uint32_t code) {
    asm volatile("cli");
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    tcb_t *task_to_terminate = (tcb_t *)g_current_task;
    KERNEL_ASSERT(task_to_terminate && task_to_terminate->pid != IDLE_TASK_PID, "Cannot terminate idle/null task");

    SCHED_INFO("Task PID %lu exiting with code %lu. Marking as ZOMBIE.", task_to_terminate->pid, code);
    task_to_terminate->state = TASK_ZOMBIE;
    task_to_terminate->exit_code = code;
    // g_task_count is decremented by scheduler_cleanup_zombies
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
    schedule();
    KERNEL_PANIC_HALT("Returned from schedule() in remove_current_task!");
}

volatile tcb_t* get_current_task_volatile(void) { return g_current_task; }
tcb_t* get_current_task(void) { return (tcb_t *)g_current_task; }
void scheduler_start(void) {
    SCHED_INFO("Scheduler marked as ready.");
    g_scheduler_ready = true;
}

void scheduler_init(void) {
    SCHED_INFO("Initializing scheduler...");
    memset(g_run_queues, 0, sizeof(g_run_queues));
    g_current_task = NULL;
    g_task_count = 0;
    g_tick_count = 0;
    g_scheduler_ready = false;
    g_all_tasks_head = NULL;

    spinlock_init(&g_scheduler_global_lock);
    spinlock_init(&g_all_tasks_lock);
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        init_run_queue(&g_run_queues[i]);
    }
    init_sleep_queue();
    scheduler_init_idle_task(); // Sets up TCB, PCB, kernel stack for idle task

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[g_idle_task_tcb.priority].lock);
    enqueue_task_locked(&g_idle_task_tcb);
    g_task_count++; // Idle task is the first active task
    spinlock_release_irqrestore(&g_run_queues[g_idle_task_tcb.priority].lock, queue_irq_flags);
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
    
    uintptr_t idle_stack_top_virt = PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);
    tss_set_kernel_stack((uint32_t)idle_stack_top_virt);
    g_current_task = &g_idle_task_tcb; // Set initial current task

    SCHED_INFO("Scheduler initialized (Idle Task PID %lu). Active tasks: %lu", IDLE_TASK_PID, g_task_count);
}

// Helper to add a task to its run queue (ensure this is robust)
// This is essentially what enqueue_task_locked does.
// If enqueue_task_locked is static and you cannot change it, replicate its logic here
// or better, make enqueue_task_locked static and create a public wrapper.
// For this fix, we'll assume a new public function or that we can call a static helper carefully.

void scheduler_unblock_task(tcb_t *task) {
    if (!task) {
        SCHED_WARN("scheduler_unblock_task: Called with NULL task.");
        return;
    }

    // Acquire global scheduler lock for consistent task state modification
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);

    if (task->state == TASK_BLOCKED) { // Only proceed if the task was actually blocked
        task->state = TASK_READY;
        SCHED_DEBUG("Task PID %lu unblocked, new state: READY.", task->pid);

        // Now add it to the appropriate run queue
        // This requires access to g_run_queues and enqueue_task_locked logic
        if (task->priority >= SCHED_PRIORITY_LEVELS) {
            SCHED_ERROR("Task PID %lu has invalid priority %u during unblock.", task->pid, task->priority);
            // Potentially reset to default or handle error
        } else {
            run_queue_t *queue = &g_run_queues[task->priority];
            uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);
            
            // Check if already in a list to prevent double enqueuing (should not happen if logic is correct)
            // This basic check is simplified; a more robust check might iterate the queue.
            if (task->next != NULL || queue->tail == task) {
                 SCHED_WARN("Task PID %lu might already be in a run queue or has invalid next pointer before unblock-enqueue.", task->pid);
                 // Decide on behavior: proceed cautiously or assert/error.
                 // For safety, let's ensure 'next' is NULL before enqueue.
                 task->next = NULL;
            }

            enqueue_task_locked(task); // Assumes this function is accessible and handles queue empty case
            
            spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
            SCHED_DEBUG("Task PID %lu enqueued into run queue Prio %u.", task->pid, task->priority);
        }
    } else {
        SCHED_WARN("scheduler_unblock_task called on task PID %lu which was not BLOCKED (state=%d).", task->pid, task->state);
    }

    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    // Optionally, if an immediate reschedule is desired after unblocking a task:
    // if (g_scheduler_ready) {
    //     schedule(); // Or set a flag for the scheduler to run soon
    // }
}