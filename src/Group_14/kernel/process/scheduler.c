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
#include <kernel/process/scheduler.h>
#include <kernel/process/process.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/sync/spinlock.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/gdt.h>
#include <kernel/lib/assert.h>
#include <kernel/memory/paging.h>
#include <kernel/cpu/tss.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/lib/port_io.h>
#include <kernel/drivers/input/keyboard_hw.h>
#include <kernel/drivers/input/keyboard.h> 
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <kernel/lib/string.h>

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
#define SCHED_INFO(fmt, ...)  serial_printf("[Sched INFO ] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) serial_printf("[Sched DEBUG] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) serial_printf("[Sched ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_WARN(fmt, ...)  serial_printf("[Sched WARN ] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define SCHED_TRACE(fmt, ...) ((void)0)


#ifndef PIC1_DATA_PORT
#define PIC1_DATA_PORT  0x21 // Master PIC IMR port
#endif

extern volatile uint32_t g_keyboard_irq_fire_count;

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
void check_idle_task_stack_integrity(const char *checkpoint);

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
    
    // Debug: Log initial register state and ESP
    uint32_t gs, fs, es, ds, current_esp;
    asm volatile(
        "mov %%gs, %0\n"
        "mov %%fs, %1\n"
        "mov %%es, %2\n"
        "mov %%ds, %3\n"
        "mov %%esp, %4\n"
        : "=r"(gs), "=r"(fs), "=r"(es), "=r"(ds), "=r"(current_esp)
    );
    serial_printf("[Idle DEBUG] Initial segment registers: GS=0x%x FS=0x%x ES=0x%x DS=0x%x\n",
                   gs & 0xFFFF, fs & 0xFFFF, es & 0xFFFF, ds & 0xFFFF);
    serial_printf("[Idle DEBUG] Current ESP after function prologue: 0x%x\n", current_esp);
    serial_printf("[Idle DEBUG] TCB saved ESP was: 0x%x\n", (uint32_t)g_idle_task_tcb.esp);

    // Ensure segment registers are properly set
    asm volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : : "ax"
    );

    uint32_t loop_count = 0;
    while (1) {
        loop_count++;
        
        // Check segments before operations
        uint32_t ds_before;
        asm volatile("mov %%ds, %0" : "=r"(ds_before));
        if ((ds_before & 0xFFFF) != 0x10) {
            serial_printf("[Idle ERROR] DS corrupted before cleanup! DS=0x%x at loop %u\n", 
                         ds_before & 0xFFFF, loop_count);
        }
        
        // Ensure segments are still correct before cleanup
        asm volatile(
            "push %%eax\n"
            "mov $0x10, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "pop %%eax\n"
            : : : "memory"
        );
        
        scheduler_cleanup_zombies(); // Periodically try to reap zombie processes
        
        // Check segments after cleanup
        uint32_t ds_after;
        asm volatile("mov %%ds, %0" : "=r"(ds_after));
        if ((ds_after & 0xFFFF) != 0x10) {
            serial_printf("[Idle ERROR] DS corrupted after cleanup! DS=0x%x at loop %u\n", 
                         ds_after & 0xFFFF, loop_count);
        }
        
        // Memory barrier to ensure all writes complete
        asm volatile("mfence" ::: "memory");

        // Enable interrupts and halt the CPU until the next interrupt.
        // This saves power and yields to other tasks if they become ready.
        asm volatile ("sti; hlt");

        // Interrupts are automatically disabled by CPU upon IRQ.
        // The IRQ handler (e.g., pit_irq_handler) will re-enable them if necessary
        // or the scheduler will when switching back to a task.
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
    // Allocate idle task stack from kmalloc to avoid static buffer issues
    void *idle_stack_mem = kmalloc(PROCESS_KSTACK_SIZE + 8); // Extra space for alignment
    if (!idle_stack_mem) {
        KERNEL_PANIC_HALT("Failed to allocate idle task stack!");
    }
    
    // Align to 16-byte boundary
    uintptr_t idle_stack_base = ((uintptr_t)idle_stack_mem + 15) & ~15;
    uintptr_t idle_stack_top = idle_stack_base + PROCESS_KSTACK_SIZE;
    
    // Explicitly zero the stack region
    serial_printf("[Sched DEBUG] Zeroing idle task stack region: V=[%p - %p)\n",
                  (void*)idle_stack_base, (void*)idle_stack_top);
    memset((void*)idle_stack_base, 0, PROCESS_KSTACK_SIZE); // Zero the usable stack size
    
    uintptr_t stack_top_virt_addr = idle_stack_top;
    g_idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)stack_top_virt_addr;

    // Log the allocated stack location
    serial_printf("[Sched DEBUG] Idle stack allocated at virt %p-%p\n", 
                  (void*)idle_stack_base, (void*)idle_stack_top);

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
    
    // Build the stack exactly as context_switch expects to find it.
    // context_switch restores in this exact order:
    // 1. POPAD - pops 8 dwords: EDI, ESI, EBP, skip ESP, EBX, EDX, ECX, EAX
    // 2. POPFD - pops EFLAGS  
    // 3. POP GS
    // 4. POP FS
    // 5. POP ES
    // 6. POP DS
    // 7. POP EBP (function epilogue)
    // 8. RET - pops return address
    //
    // We build the stack from high to low addresses using --kstack_ptr.
    // What we store FIRST will be at the HIGHEST address (popped LAST).
    // What we store LAST will be at the LOWEST address (ESP points here, popped FIRST).
    
    // Store in reverse order of how they'll be popped:
    *(--kstack_ptr) = (uint32_t)kernel_idle_task_loop; // Return address for RET
    *(--kstack_ptr) = 0; // Saved EBP for epilogue  
    // Segment registers - same order as they appear after context save
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // DS (first to be popped)
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // ES
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // FS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // GS (last to be popped)
    *(--kstack_ptr) = 0x00000202; // EFLAGS with interrupts enabled
    
    // 1. POPAD expects 8 dwords on stack in this order (from low addr to high):
    // EDI at [ESP+0], ESI at [ESP+4], EBP at [ESP+8], ESP at [ESP+12] (skipped),
    // EBX at [ESP+16], EDX at [ESP+20], ECX at [ESP+24], EAX at [ESP+28]
    *(--kstack_ptr) = 0; // EAX (will be at ESP+28)
    *(--kstack_ptr) = 0; // ECX (will be at ESP+24)
    *(--kstack_ptr) = 0; // EDX (will be at ESP+20)
    *(--kstack_ptr) = 0; // EBX (will be at ESP+16)
    *(--kstack_ptr) = 0; // ESP placeholder (will be at ESP+12, skipped by POPAD)
    *(--kstack_ptr) = 0; // EBP (will be at ESP+8)
    *(--kstack_ptr) = 0; // ESI (will be at ESP+4)
    *(--kstack_ptr) = 0; // EDI (will be at ESP+0, first register restored)
    
    g_idle_task_tcb.esp = kstack_ptr;
    SCHED_DEBUG("Idle task initial TCB ESP set to: %p", g_idle_task_tcb.esp);
    
    // Debug: Verify stack contents match what context_switch expects
    SCHED_DEBUG("Idle task stack contents (ESP=%p):", kstack_ptr);
    uint32_t *debug_ptr = kstack_ptr;
    SCHED_DEBUG("Context restore order: POPAD (8 regs), POPFD, POP GS/FS/ES/DS, POP EBP, RET");
    SCHED_DEBUG("  [ESP+0]  EDI = 0x%08lx", (unsigned long)debug_ptr[0]);
    SCHED_DEBUG("  [ESP+4]  ESI = 0x%08lx", (unsigned long)debug_ptr[1]);
    SCHED_DEBUG("  [ESP+8]  EBP = 0x%08lx", (unsigned long)debug_ptr[2]);
    SCHED_DEBUG("  [ESP+12] ESP = 0x%08lx (skipped by POPAD)", (unsigned long)debug_ptr[3]);
    SCHED_DEBUG("  [ESP+16] EBX = 0x%08lx", (unsigned long)debug_ptr[4]);
    SCHED_DEBUG("  [ESP+20] EDX = 0x%08lx", (unsigned long)debug_ptr[5]);
    SCHED_DEBUG("  [ESP+24] ECX = 0x%08lx", (unsigned long)debug_ptr[6]);
    SCHED_DEBUG("  [ESP+28] EAX = 0x%08lx", (unsigned long)debug_ptr[7]);
    SCHED_DEBUG("  [ESP+32] EFLAGS = 0x%08lx (IF=%s)", (unsigned long)debug_ptr[8], 
                (debug_ptr[8] & 0x200) ? "1" : "0");
    SCHED_DEBUG("  [ESP+36] DS = 0x%08lx (expect 0x10)", (unsigned long)debug_ptr[9]);
    SCHED_DEBUG("  [ESP+40] ES = 0x%08lx (expect 0x10)", (unsigned long)debug_ptr[10]);
    SCHED_DEBUG("  [ESP+44] FS = 0x%08lx (expect 0x10)", (unsigned long)debug_ptr[11]);
    SCHED_DEBUG("  [ESP+48] GS = 0x%08lx (expect 0x10)", (unsigned long)debug_ptr[12]);
    SCHED_DEBUG("  [ESP+52] saved EBP = 0x%08lx", (unsigned long)debug_ptr[13]);
    SCHED_DEBUG("  [ESP+56] return addr = 0x%08lx (kernel_idle_task_loop)", (unsigned long)debug_ptr[14]);

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
        
        // Check idle task stack before destroying process
        check_idle_task_stack_integrity("Before destroy_process");
        
        if (zombie_to_reap->process) {
            serial_printf("[destroy_process] Enter for PID %lu\n", zombie_to_reap->pid);
            destroy_process(zombie_to_reap->process);
            serial_printf("[destroy_process] Exit for PID %lu\n", zombie_to_reap->pid);
        }
        else SCHED_WARN("Zombie task PID %lu has NULL process pointer!", zombie_to_reap->pid);
        
        // Check idle task stack after destroying process
        check_idle_task_stack_integrity("After destroy_process");
        
        kfree(zombie_to_reap);
        
        // Check idle task stack after freeing TCB
        check_idle_task_stack_integrity("After kfree(tcb)");
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
    
    // Debug: Log ESP values before switch
    if (old_task && old_task->pid == IDLE_TASK_PID) {
        serial_printf("[Sched DEBUG] Switching FROM idle task, current ESP will be saved to: %p\n", &(old_task->esp));
    }
    if (new_task && new_task->pid == IDLE_TASK_PID) {
        serial_printf("[Sched DEBUG] Switching TO idle task, saved ESP is: %p\n", new_task->esp);
    }
    
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
        
        // Debug: Verify stack contents before switch to idle task
        if (new_task->pid == IDLE_TASK_PID) {
            uint32_t *stack_ptr = (uint32_t*)new_task->esp;
            serial_printf("[Sched DEBUG] Pre-switch idle stack check (ESP=%p):\n", stack_ptr);
            
            // First, let's see what's actually on the stack
            serial_printf("[Sched DEBUG] Stack dump (looking for 0x10 pattern):\n");
            for (int i = 0; i < 20; i++) {
                serial_printf("  [ESP+%d] = 0x%08x %s\n", i*4, stack_ptr[i],
                              stack_ptr[i] == 0x10 ? "<-- KERNEL_DATA_SEL" : "");
            }
            
            serial_printf("  [ESP+36] DS value = 0x%08x (expect 0x10)\n", stack_ptr[9]);
            serial_printf("  [ESP+40] ES value = 0x%08x (expect 0x10)\n", stack_ptr[10]);
            serial_printf("  [ESP+44] FS value = 0x%08x (expect 0x10)\n", stack_ptr[11]);
            serial_printf("  [ESP+48] GS value = 0x%08x (expect 0x10)\n", stack_ptr[12]);
        }
        
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

//============================================================================
// Debug Helper Functions
//============================================================================
void check_idle_task_stack_integrity(const char *checkpoint) {
    if (!g_idle_task_tcb.esp) return;
    
    uint32_t *stack_ptr = (uint32_t*)g_idle_task_tcb.esp;
    
    // Check if ESP is within valid range
    uintptr_t stack_base = (uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top - PROCESS_KSTACK_SIZE;
    uintptr_t stack_top = (uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top;
    
    if ((uintptr_t)stack_ptr < stack_base || (uintptr_t)stack_ptr >= stack_top) {
        serial_printf("[Stack Check] %s: Idle ESP %p out of range [%p-%p)\n", 
                      checkpoint, stack_ptr, (void*)stack_base, (void*)stack_top);
        return;
    }
    
    // Check segment registers at expected positions
    bool corrupted = false;
    if (stack_ptr[9] != KERNEL_DATA_SELECTOR) {
        serial_printf("[Stack Check] %s: Idle DS corrupted: 0x%x (expect 0x10)\n", 
                      checkpoint, stack_ptr[9]);
        corrupted = true;
    }
    if (stack_ptr[10] != KERNEL_DATA_SELECTOR) {
        serial_printf("[Stack Check] %s: Idle ES corrupted: 0x%x (expect 0x10)\n", 
                      checkpoint, stack_ptr[10]);
        corrupted = true;
    }
    if (stack_ptr[11] != KERNEL_DATA_SELECTOR) {
        serial_printf("[Stack Check] %s: Idle FS corrupted: 0x%x (expect 0x10)\n", 
                      checkpoint, stack_ptr[11]);
        corrupted = true;
    }
    if (stack_ptr[12] != KERNEL_DATA_SELECTOR) {
        serial_printf("[Stack Check] %s: Idle GS corrupted: 0x%x (expect 0x10)\n", 
                      checkpoint, stack_ptr[12]);
        corrupted = true;
    }
    
    if (corrupted) {
        serial_printf("[Stack Check] %s: CORRUPTION DETECTED! Dumping stack:\n", checkpoint);
        for (int i = 0; i < 20; i++) {
            serial_printf("  [ESP+%d] = 0x%08x\n", i*4, stack_ptr[i]);
        }
    }
}

void scheduler_start(void) {
    terminal_printf("Scheduler starting...\n");
    g_scheduler_ready = true;
    g_need_reschedule = false; // Clear any pending reschedule from init

    tcb_t *first_task = scheduler_select_next_task();
    KERNEL_ASSERT(first_task != NULL, "scheduler_start: No task to run!");

    g_current_task = first_task;
    g_current_task->state = TASK_RUNNING;
    g_current_task->has_run = true; // Mark as having run

    terminal_printf("  [Scheduler Start] First task selected: PID %lu (ESP=%p)\n",
                     (unsigned long)g_current_task->pid, g_current_task->esp);

    // Set TSS ESP0 for the first task.
    // Note: For the idle task, kernel_stack_vaddr_top is set in scheduler_init_idle_task.
    // For user tasks, it's set in allocate_kernel_stack.
    KERNEL_ASSERT(g_current_task->process && g_current_task->process->kernel_stack_vaddr_top,
                  "First task's PCB or kernel_stack_vaddr_top is NULL");
    tss_set_kernel_stack((uint32_t)g_current_task->process->kernel_stack_vaddr_top);

    if (g_current_task->pid != IDLE_TASK_PID) {
        // First task is a user process
        terminal_printf("  [Scheduler Start] Jumping to user mode for PID %lu.\n", (unsigned long)g_current_task->pid);
        jump_to_user_mode(g_current_task->esp, g_current_task->process->page_directory_phys);
    } else {
        // First task is the Idle Task. Its ESP points to a kernel stack frame.
        // We effectively switch from the current "bootstrap" kernel context to the idle task's context.
        // The context_switch function needs to handle `old_esp_ptr == NULL`.
        terminal_printf("  [Scheduler Start] Context switching to Idle Task (PID %lu).\n", (unsigned long)g_current_task->pid);
        context_switch(NULL, g_current_task->esp, g_current_task->process->page_directory_phys);
    }

    // These lines should not be reached if the switch/jump is successful.
    KERNEL_PANIC_HALT("scheduler_start: Initial task switch/jump failed to transfer control!");
}

void scheduler_init(void) {
    terminal_printf("Initializing scheduler...\n");
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

    // Do NOT set g_current_task = &g_idle_task_tcb here.
    // It will be set properly in scheduler_start() when we do the first context switch.

    terminal_printf("Scheduler initialized\n");
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