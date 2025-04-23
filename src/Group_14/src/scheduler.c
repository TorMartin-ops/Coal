/**
 * @file scheduler.c
 * @brief Advanced Priority-Based Preemptive Kernel Scheduler
 * @version 4.2 (Implements zombie cleanup via global task list)
 *
 * @details Implements a priority-based preemptive scheduler for UiAOS.
 * Features multiple run queues (one per priority level), configurable time slices,
 * task states including SLEEPING, periodic checking of sleeping tasks,
 * and basic SMP preparation via per-queue locking. Includes zombie task cleanup.
 *
 * Assumes a timer interrupt calls scheduler_tick() periodically.
 */

//============================================================================
// Includes
//============================================================================

#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <string.h>      // memset

#include "scheduler.h"      // Public interface & TCB definition
#include "process.h"        // pcb_t, destroy_process, PROCESS_KSTACK_SIZE
#include "kmalloc.h"        // kmalloc, kfree
#include "terminal.h"       // terminal_printf, terminal_write
#include "spinlock.h"       // spinlock_t, spinlock functions
#include "idt.h"            // Interrupt control (for locks)
#include "gdt.h"            // GDT selectors (KERNEL_CS/DS)
#include "assert.h"         // KERNEL_ASSERT, KERNEL_PANIC_HALT
#include "paging.h"         // g_kernel_page_directory_phys
#include "tss.h"            // tss_set_kernel_stack
#include "serial.h"         // Serial port logging for critical paths
#include "pit.h"            // get_pit_ticks() - Or a generic timer interface

//============================================================================
// Scheduler Configuration & Constants
//============================================================================

#define SCHED_PRIORITY_LEVELS   4       // Number of priority levels (0=highest)
#define SCHED_DEFAULT_PRIORITY  1       // Default priority for new user tasks
#define SCHED_IDLE_PRIORITY     (SCHED_PRIORITY_LEVELS - 1) // Idle task gets the lowest priority
#define SCHED_KERNEL_PRIORITY   0       // Priority for important kernel tasks (highest)

// Define SCHED_TICKS_PER_SECOND based on your PIT/Timer frequency (e.g., 1000 for 1ms tick)
#ifndef SCHED_TICKS_PER_SECOND
#define SCHED_TICKS_PER_SECOND  1000    // Assume 1000 Hz timer interrupt rate
#endif

// Convert milliseconds to tick counts using the defined frequency
#define MS_TO_TICKS(ms) (((ms) * SCHED_TICKS_PER_SECOND) / 1000)

// Configurable time slices per priority level (in milliseconds)
static const uint32_t g_priority_time_slices_ms[SCHED_PRIORITY_LEVELS] = {
    200,  // Priority 0 (Kernel): 200ms
    100,  // Priority 1 (Default User): 100ms
    50,   // Priority 2: 50ms
    25    // Priority 3 (Idle): 25ms (effectively runs whenever nothing else is ready)
};

// --- Kernel Error Codes ---
#define SCHED_OK         0
#define SCHED_ERR_NOMEM  (-1) // Out of memory
#define SCHED_ERR_FAIL   (-2) // General failure
#define SCHED_ERR_INVALID (-3) // Invalid parameter
#define SCHED_ERR_NOENT  (-4) // No such task

// --- Physical/Virtual Address Conversion (Example) ---
#ifndef KERNEL_PHYS_BASE
#define KERNEL_PHYS_BASE 0x100000u
#endif
#ifndef KERNEL_VIRT_BASE
#define KERNEL_VIRT_BASE 0xC0100000u
#endif
#define PHYS_TO_VIRT(p) ((uintptr_t)(p) >= KERNEL_PHYS_BASE ? \
                         ((uintptr_t)(p) - KERNEL_PHYS_BASE + KERNEL_VIRT_BASE) : \
                         (uintptr_t)(p))

// --- Debug Logging Macros ---
#define SCHEDULER_DEBUG 1 // Set to 0 to disable detailed debug output

// Use %lu for uint32_t, %llu for uint64_t, %d for int, %u for unsigned int/enum
#define SCHED_INFO(fmt, ...)  terminal_printf("[Sched INFO ] " fmt "\n", ##__VA_ARGS__)
#if SCHEDULER_DEBUG >= 2
#define SCHED_TRACE(fmt, ...) serial_printf("[Sched TRACE] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define SCHED_TRACE(fmt, ...) ((void)0)
#endif
#if SCHEDULER_DEBUG >= 1
#define SCHED_DEBUG(fmt, ...) terminal_printf("[Sched DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define SCHED_DEBUG(fmt, ...) ((void)0)
#endif
#define SCHED_ERROR(fmt, ...) terminal_printf("[Sched ERROR] " fmt "\n", ##__VA_ARGS__)


//============================================================================
// Data Structures
//============================================================================

// --- Task States --- defined in scheduler.h

// --- Task Control Block (TCB) --- defined in scheduler.h

/** @brief Structure representing a run queue for a specific priority level. */
typedef struct {
    tcb_t      *head;         ///< Head of the singly linked list for this priority.
    tcb_t      *tail;         ///< Tail of the singly linked list for this priority.
    uint32_t    count;         ///< Number of tasks currently in this queue.
    spinlock_t  lock;          ///< Lock protecting this specific queue (for SMP readiness).
} run_queue_t;

/** @brief Structure representing the queue of sleeping tasks. */
typedef struct {
    tcb_t      *head;         ///< Head of the list (sorted by wakeup_time). Uses wait_next/prev pointers.
    uint32_t    count;         ///< Number of tasks currently sleeping.
    spinlock_t  lock;          ///< Lock protecting the sleep queue.
} sleep_queue_t;

//============================================================================
// Module Static Data
//============================================================================

/** @brief Array of run queues, one for each priority level. */
static run_queue_t g_run_queues[SCHED_PRIORITY_LEVELS];

/** @brief Queue holding tasks currently in the TASK_SLEEPING state. */
static sleep_queue_t g_sleep_queue;

/** @brief Pointer to the Task Control Block of the currently executing task. Must be volatile. */
static volatile tcb_t *g_current_task = NULL;

/** @brief Total number of active (non-ZOMBIE) tasks managed by the scheduler. */
static uint32_t g_task_count = 0;

/** @brief Counter for the number of context switches performed (for stats). */
static uint32_t g_context_switches = 0;

/** @brief Global lock for operations affecting scheduler-wide state or multiple queues. */
static spinlock_t g_scheduler_global_lock;

/** @brief Global system tick counter, incremented by scheduler_tick(). */
static volatile uint32_t g_tick_count = 0;

/** @brief TCB for the dedicated kernel idle task. */
static tcb_t g_idle_task_tcb;

/** @brief Minimal PCB associated with the idle task. */
static pcb_t g_idle_task_pcb;

/** @brief Head of the linked list containing ALL tasks (regardless of state). */
static tcb_t *g_all_tasks_head = NULL; // <<< ADDED for cleanup

/** @brief Lock protecting the global task list. */
static spinlock_t g_all_tasks_lock; // <<< ADDED for cleanup

/** @brief Flag indicating if the scheduler has been initialized and started. */
volatile bool g_scheduler_ready = false; // Definition (extern in header)

//============================================================================
// Forward Declarations (Internal Functions)
//============================================================================

// Task Selection & Switching
static tcb_t* scheduler_select_next_task(void);
static void   perform_context_switch(tcb_t *old_task, tcb_t *new_task);
extern void   context_switch(uint32_t **old_esp_ptr, uint32_t *new_esp, uint32_t *new_pagedir); // In context_switch.asm
extern void   jump_to_user_mode(uint32_t *user_esp, uint32_t *pagedir); // In jump_user.asm


// Queue Management
static void   init_run_queue(run_queue_t *queue);
static void   init_sleep_queue(void);
static void   enqueue_task_locked(tcb_t *task); // Adds to appropriate run queue
static bool   dequeue_task_locked(tcb_t *task); // Correct return type
static void   add_to_sleep_queue_locked(tcb_t *task);
static void   remove_from_sleep_queue_locked(tcb_t *task);
static void   check_sleeping_tasks_locked(void);

// Idle Task & Cleanup
static void   kernel_idle_task_loop(void) __attribute__((noreturn));
static void   scheduler_init_idle_task(void);
void          scheduler_cleanup_zombies(void); // Now a public-like internal function called by idle task

// Debugging
static void   dump_scheduler_state(void); // Optional debug dump function

//============================================================================
// Queue Management Functions (Internal) - DEFINITIONS START HERE
//============================================================================

/** @brief Initializes a run queue structure. */
static void init_run_queue(run_queue_t *queue) {
    KERNEL_ASSERT(queue != NULL, "NULL queue passed to init");
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    spinlock_init(&queue->lock);
}

/** @brief Initializes the sleep queue structure. */
static void init_sleep_queue(void) {
    g_sleep_queue.head = NULL;
    g_sleep_queue.count = 0;
    spinlock_init(&g_sleep_queue.lock);
}

/**
 * @brief Adds a task to the tail of its priority run queue.
 * @param task The task to enqueue (must have valid priority).
 * @note Assumes the specific run queue's lock is held.
 */
static void enqueue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot enqueue NULL task");
    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority");

    run_queue_t *queue = &g_run_queues[task->priority];
    SCHED_TRACE("Enqueue PID %lu (Prio %u) - Head=%p Tail=%p Count=%lu",
        task->pid, task->priority, queue->head, queue->tail, (unsigned long)queue->count);

    // Add to tail
    task->next = NULL; // Task becomes the new tail
    if (queue->tail) {
        queue->tail->next = task;
        queue->tail = task;
    } else {
        // Queue was empty
        queue->head = task;
        queue->tail = task;
    }
    queue->count++;
}

/**
 * @brief Removes a task from its priority run queue.
 * @param task The task to dequeue.
 * @note Assumes the specific run queue's lock is held.
 * @return true if found and removed, false otherwise.
 */
static bool dequeue_task_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot dequeue NULL task");
    KERNEL_ASSERT(task->priority < SCHED_PRIORITY_LEVELS, "Invalid task priority");

    run_queue_t *queue = &g_run_queues[task->priority];
    SCHED_TRACE("Dequeue PID %lu (Prio %u) - Head=%p Tail=%p Count=%lu",
        task->pid, task->priority, queue->head, queue->tail, (unsigned long)queue->count);

    if (!queue->head) return false; // Queue empty

    // Handle removal from head
    if (queue->head == task) {
        queue->head = task->next;
        if (queue->tail == task) { // Was it the only task?
            queue->tail = NULL;
        }
        // Check queue count before decrementing
        if (queue->count == 0) {
             SCHED_ERROR("Run queue count inconsistency (head removal) for Prio %u", task->priority);
             // KERNEL_PANIC_HALT("Queue count error"); // Optionally panic
        } else {
             queue->count--;
        }
        task->next = NULL; // Clear link
        return true;
    }

    // Search for task in the rest of the list
    tcb_t *prev = queue->head;
    while (prev->next && prev->next != task) {
        prev = prev->next;
    }

    // If found, unlink it
    if (prev->next == task) {
        prev->next = task->next;
        if (queue->tail == task) { // Was it the tail?
            queue->tail = prev;
        }
         // Check queue count before decrementing
        if (queue->count == 0) {
             SCHED_ERROR("Run queue count inconsistency (mid removal) for Prio %u", task->priority);
             // KERNEL_PANIC_HALT("Queue count error"); // Optionally panic
        } else {
             queue->count--;
        }
        task->next = NULL; // Clear link
        return true;
    }

    SCHED_ERROR("Task PID %lu not found in its run queue (Prio %u) for dequeue!",
                 task->pid, task->priority);
    return false; // Task not found
}


/**
 * @brief Adds a task to the sleep queue, sorted by wakeup time.
 * @param task Pointer to the TCB to add.
 * @note Assumes the sleep queue lock is held.
 */
static void add_to_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot add NULL task to sleep queue");
    KERNEL_ASSERT(task->state == TASK_SLEEPING, "Task not in SLEEPING state");
    SCHED_TRACE("Adding PID %lu to sleep queue (Wakeup: %lu)", task->pid, (unsigned long)task->wakeup_time);

    task->wait_next = NULL;
    task->wait_prev = NULL;

    // Empty list or insert at head
    if (!g_sleep_queue.head || task->wakeup_time <= g_sleep_queue.head->wakeup_time) { // Use <= to ensure FIFO for same wakeup time
        task->wait_next = g_sleep_queue.head;
        if (g_sleep_queue.head) {
            g_sleep_queue.head->wait_prev = task;
        }
        g_sleep_queue.head = task;
    } else {
        // Find insertion point
        tcb_t *current = g_sleep_queue.head;
        while (current->wait_next && current->wait_next->wakeup_time <= task->wakeup_time) {
            current = current->wait_next;
        }
        // Insert after current
        task->wait_next = current->wait_next;
        task->wait_prev = current;
        if (current->wait_next) {
            current->wait_next->wait_prev = task;
        }
        current->wait_next = task;
    }
    g_sleep_queue.count++;
}

/**
 * @brief Removes a task from the sleep queue.
 * @param task Pointer to the TCB to remove.
 * @note Assumes the sleep queue lock is held.
 */
static void remove_from_sleep_queue_locked(tcb_t *task) {
    KERNEL_ASSERT(task != NULL, "Cannot remove NULL task from sleep queue");
    SCHED_TRACE("Removing PID %lu from sleep queue", task->pid);

    // Fix predecessor's next pointer
    if (task->wait_prev) {
        task->wait_prev->wait_next = task->wait_next;
    } else if (g_sleep_queue.head == task) { // Was head
        g_sleep_queue.head = task->wait_next;
    }

    // Fix successor's prev pointer
    if (task->wait_next) {
        task->wait_next->wait_prev = task->wait_prev;
    }

    // Clear task's pointers and decrement count
    task->wait_next = NULL;
    task->wait_prev = NULL;
    if (g_sleep_queue.count > 0) {
        g_sleep_queue.count--;
    } else {
        SCHED_ERROR("Sleep queue count inconsistency during removal!");
    }
}

/**
 * @brief Checks the sleep queue and wakes up tasks whose time has come.
 * @note Assumes the sleep queue lock is held.
 */
static void check_sleeping_tasks_locked(void) {
    if (!g_sleep_queue.head) return; // No tasks sleeping

    uint32_t current_ticks = g_tick_count; // Use volatile global tick count

    SCHED_TRACE("Checking sleep queue at tick %lu...", (unsigned long)current_ticks);

    tcb_t *task = g_sleep_queue.head;
    while (task && task->wakeup_time <= current_ticks) {
        tcb_t *task_to_wake = task;
        task = task->wait_next; // Advance iterator before modifying list

        SCHED_DEBUG("Waking up task PID %lu (was sleeping until tick %lu)",
                   task_to_wake->pid, (unsigned long)task_to_wake->wakeup_time);

        // Remove from sleep queue (caller holds lock)
        remove_from_sleep_queue_locked(task_to_wake);

        // Mark as ready and add to appropriate run queue
        task_to_wake->state = TASK_READY;
        // Need run queue lock for this
        uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[task_to_wake->priority].lock);
        enqueue_task_locked(task_to_wake);
        spinlock_release_irqrestore(&g_run_queues[task_to_wake->priority].lock, queue_irq_flags);
    }
}

//============================================================================
// Time Management & Tick Handler
//============================================================================

/**
 * @brief Returns the current system tick count.
 * @return The volatile tick count.
 */
uint32_t scheduler_get_ticks(void) {
    // Volatile read
    return g_tick_count;
}

/**
 * @brief Scheduler's routine to be called by the timer interrupt handler.
 * @details Increments the global tick count, checks for sleeping tasks,
 * manages the current task's time slice, and triggers preemption if needed.
 * @note Assumes it's called with interrupts disabled.
 */
 void scheduler_tick(void) {
    serial_write("[Sched] Enter scheduler_tick\n"); // <<< ADDED LOG

    // Increment global tick count first
    // Use atomic operation if SMP
    g_tick_count++;

    // Check if scheduler is active
    if (!g_scheduler_ready) return;

    // --- Check Sleep Queue ---
    serial_write("[Sched Tick] Before acquiring global lock\n"); // <<< ADDED LOG
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    serial_write("[Sched Tick] Acquired global lock\n"); // <<< ADDED LOG

    serial_write("[Sched Tick] Before acquiring sleep lock\n"); // <<< ADDED LOG
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    serial_write("[Sched Tick] Acquired sleep lock\n"); // <<< ADDED LOG

    serial_write("[Sched Tick] Before check_sleeping_tasks_locked\n"); // <<< ADDED LOG
    check_sleeping_tasks_locked();
    serial_write("[Sched Tick] After check_sleeping_tasks_locked\n"); // <<< ADDED LOG

    serial_write("[Sched Tick] Before releasing sleep lock\n"); // <<< ADDED LOG
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
    serial_write("[Sched Tick] Released sleep lock\n"); // <<< ADDED LOG

    serial_write("[Sched Tick] Before releasing global lock\n"); // <<< ADDED LOG
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
    serial_write("[Sched Tick] Released global lock\n"); // <<< ADDED LOG


    // --- Manage Current Task's Time Slice ---
    serial_write("[Sched Tick] Before reading g_current_task\n"); // <<< ADDED LOG
    volatile tcb_t *curr_task_volatile = g_current_task; // Read volatile pointer once
    serial_write("[Sched Tick] After reading g_current_task\n"); // <<< ADDED LOG

    if (!curr_task_volatile) return; // No task running

    // Cast to non-volatile *after* checking for NULL.
    tcb_t *curr_task = (tcb_t*)curr_task_volatile;

    // If the current task is the idle task, just call schedule to see if anything else is ready
    if (curr_task->pid == IDLE_TASK_PID) {
        serial_write("[Sched Tick] Calling schedule() for IDLE\n"); // <<< ADDED LOG
        schedule(); // Check if higher priority tasks are ready
        return;
    }

    // --- Logic for non-idle tasks ---
    serial_write("[Sched Tick] Processing non-idle task slice\n"); // <<< ADDED LOG

    // Update task's total runtime
    curr_task->runtime_ticks++;

    // Decrement remaining ticks in current slice
    if (curr_task->ticks_remaining > 0) {
        curr_task->ticks_remaining--;
    }

    // If time slice expired, trigger preemption by calling schedule
    if (curr_task->ticks_remaining == 0) {
        SCHED_TRACE("Time slice expired for PID %lu. Triggering schedule.", curr_task->pid);
        serial_write("[Sched Tick] Timeslice expired, calling schedule()\n"); // <<< ADDED LOG
        schedule();
    }
    // Optional: Consider always calling schedule() here if strict preemption
    // of lower-priority tasks by newly-ready higher-priority tasks is desired,
    // even if the current task's timeslice hasn't expired.
    // else {
    //    serial_write("[Sched Tick] Timeslice OK, calling schedule() anyway\n"); // Add log if uncommenting
    //    schedule();
    // }
}


//============================================================================
// Idle Task Implementation & Zombie Cleanup
//============================================================================


/**
 * @brief The main loop for the kernel's idle task.
 */
 static __attribute__((noreturn)) void kernel_idle_task_loop(void) {
    // ---> ADDED: Log entry into idle loop <---
    serial_write("[Idle Loop] Entered.\n");
    // ---> END ADD <---

    SCHED_INFO("Idle task started (PID %lu). Entering HLT loop.", (unsigned long)IDLE_TASK_PID);
    while (1) {
        // ---> ADDED: Log before cleanup <---
        serial_write("[Idle Loop] Calling scheduler_cleanup_zombies...\n");
        // ---> END ADD <---

        scheduler_cleanup_zombies(); // Periodically clean up zombies

        // ---> ADDED: Log after cleanup <---
        serial_write("[Idle Loop] Returned from scheduler_cleanup_zombies. Executing sti; hlt\n");
        // ---> END ADD <---

        asm volatile ("sti; hlt"); // Atomically enable interrupts and halt

        // ---> ADDED: Log after waking from hlt <---
        // This will only print if an interrupt woke the CPU
        serial_write("[Idle Loop] Woke up from hlt.\n");
        // ---> END ADD <---
    }
}

/**
 * @brief Initializes the TCB and minimal PCB for the dedicated idle task.
 * CORRECTED stack setup order.
 */
 static void scheduler_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");

    // Setup Minimal PCB
    memset(&g_idle_task_pcb, 0, sizeof(pcb_t));
    g_idle_task_pcb.pid = IDLE_TASK_PID;
    g_idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    KERNEL_ASSERT(g_idle_task_pcb.page_directory_phys != NULL, "Kernel PD phys NULL");
    g_idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop; // Correct entry point
    g_idle_task_pcb.user_stack_top = NULL;
    g_idle_task_pcb.mm = NULL;

    // Setup Kernel Stack (Static buffer)
    static uint8_t idle_stack_buffer[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
    KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 512, "Idle task stack too small");
    g_idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)((uintptr_t)idle_stack_buffer + sizeof(idle_stack_buffer));
    uintptr_t idle_stack_virt_top = PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);

    // Setup TCB
    memset(&g_idle_task_tcb, 0, sizeof(tcb_t));
    g_idle_task_tcb.process = &g_idle_task_pcb;
    g_idle_task_tcb.pid     = IDLE_TASK_PID;
    g_idle_task_tcb.state   = TASK_READY;
    g_idle_task_tcb.has_run = true; // Mark as 'run'
    g_idle_task_tcb.priority = SCHED_IDLE_PRIORITY;
    g_idle_task_tcb.time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[SCHED_IDLE_PRIORITY]);
    g_idle_task_tcb.ticks_remaining = g_idle_task_tcb.time_slice_ticks;
    g_idle_task_tcb.all_tasks_next = NULL;

    // Prepare Initial Stack Pointer (ESP) for Idle Task
    uint32_t *kstack_ptr = (uint32_t*)idle_stack_virt_top; // Start at the virtual top
    uintptr_t kstack_base_virt = PHYS_TO_VIRT((uintptr_t)idle_stack_buffer);

    // Create stack frame matching the REVERSE of the context_switch POP order
    // ASM POP Order: popad -> popfd -> pop gs -> pop fs -> pop es -> pop ds -> pop ebp -> ret

    // --- FINAL Corrected Setup Order ---

    // 1. Return Address (popped LAST by 'ret')
    *(--kstack_ptr) = (uint32_t)g_idle_task_pcb.entry_point; // Fake return address is idle loop start

    // 2. Caller's EBP (popped before 'ret' by 'pop ebp')
    *(--kstack_ptr) = 0; // Fake caller's EBP (use 0 for initial frame)

    // 3. Segments popped before EBP
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // Value for DS pop
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // Value for ES pop
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // Value for FS pop
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // Value for GS pop

    // 4. EFLAGS popped before segments
    *(--kstack_ptr) = 0x00000202; // Value for POPFD (IF=1)

    // 5. PUSHAD Registers popped FIRST
    //    Order POPPED by POPAD: EAX, ECX, EDX, EBX, ESP_dummy, EBP, ESI, EDI
    //    Order setting up stack (reverse): EDI, ESI, EBP, ESP_dummy, EBX, EDX, ECX, EAX
    *(--kstack_ptr) = 0; // Value for EDI pop
    *(--kstack_ptr) = 0; // Value for ESI pop
    *(--kstack_ptr) = 0; // Value for EBP pop (from POPAD - this is the idle task's initial EBP value)
    *(--kstack_ptr) = 0; // Value for ESP_dummy pop
    *(--kstack_ptr) = 0; // Value for EBX pop
    *(--kstack_ptr) = 0; // Value for EDX pop
    *(--kstack_ptr) = 0; // Value for ECX pop
    *(--kstack_ptr) = 0; // Value for EAX pop
    // --- End Stack Setup ---

    KERNEL_ASSERT((uintptr_t)kstack_ptr >= kstack_base_virt, "Idle stack underflow during init");
    g_idle_task_tcb.esp = kstack_ptr; // Set initial ESP for context switch

    // ESP should point to the value that will be popped into EAX by popad
    SCHED_DEBUG("Idle task TCB initialized: ESP=%p (Corrected Stack Order + Ret/EBP)", g_idle_task_tcb.esp);

    // Add idle task to the global task list (Needs lock)
    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    g_idle_task_tcb.all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = &g_idle_task_tcb;
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);
}

/**
 * @brief Frees resources associated with ZOMBIE tasks.
 * @details Iterates through the global task list, finds one ZOMBIE task,
 * unlinks it, frees its TCB and associated PCB resources, and
 * decrements the global task count. It only reaps one task per call
 * to avoid holding locks for too long. Called periodically by the idle task.
 */
void scheduler_cleanup_zombies(void) { // <<< IMPLEMENTED
    SCHED_TRACE("Starting zombie cleanup check...");
    tcb_t *zombie_to_reap = NULL;
    tcb_t *prev = NULL;
    bool found_zombie = false;

    // --- Find a Zombie Task (Locked) ---
    // Acquire lock for the global task list
    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);

    tcb_t *current = g_all_tasks_head;
    while (current) {
        // Skip the idle task itself - it should never be zombie
        if (current->pid == IDLE_TASK_PID) {
            prev = current;
            current = current->all_tasks_next;
            continue;
        }

        if (current->state == TASK_ZOMBIE) {
            zombie_to_reap = current;
            found_zombie = true;

            // Unlink from the global list
            if (prev) {
                prev->all_tasks_next = current->all_tasks_next;
            } else {
                g_all_tasks_head = current->all_tasks_next; // It was the head
            }
            // Clear the zombie's link just in case
            zombie_to_reap->all_tasks_next = NULL;

            // Decrement the global task count (protected by scheduler global lock)
            // Acquire the global scheduler lock briefly just for the count.
            // We do this *after* unlinking but *before* releasing the all_tasks_lock
            // to ensure atomicity of unlink+count decrement relative to additions.
            uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
            if (g_task_count > 0) { // Ensure count doesn't go below zero
                 g_task_count--;
            } else {
                 SCHED_ERROR("Zombie cleanup attempting to decrement task count below zero!");
            }
            spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);


            break; // Found one, stop searching for this call
        }
        prev = current;
        current = current->all_tasks_next;
    }

    // Release the global task list lock *before* freeing memory
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);
    // --- End Locked Section ---


    // --- Reap the Zombie (if found) ---
    if (found_zombie && zombie_to_reap) {
        SCHED_INFO("Cleanup: Reaping ZOMBIE task PID %lu.", zombie_to_reap->pid);

        // 1. Get the associated PCB
        pcb_t *pcb_to_free = zombie_to_reap->process;

        // 2. Remember the TCB pointer itself
        tcb_t *tcb_to_free = zombie_to_reap;
        uint32_t freed_pid = tcb_to_free->pid; // Capture PID before freeing TCB

        // 3. Destroy the process resources (memory, handles, etc.)
        //    This function should handle the case where pcb_to_free is NULL,
        //    though it shouldn't be for a normal task.
        if (pcb_to_free) {
            SCHED_DEBUG("Calling destroy_process for PID %lu", pcb_to_free->pid);
            destroy_process(pcb_to_free); // Assumes destroy_process frees the PCB itself
        } else {
            SCHED_ERROR("Zombie task PID %lu had a NULL PCB pointer!", freed_pid);
        }

        // 4. Free the TCB structure itself
        SCHED_DEBUG("Freeing TCB for former PID %lu", freed_pid);
        kfree(tcb_to_free); // Assumes kfree is safe here

        SCHED_TRACE("Finished reaping ZOMBIE PID %lu.", freed_pid);

    } else {
        SCHED_TRACE("No zombie tasks found to reap this cycle.");
    }
}


//============================================================================
// Task Selection & Context Switching - DEFINITIONS START HERE
//============================================================================

/**
 * @brief Selects the next task to run based on priority.
 */
static tcb_t* scheduler_select_next_task(void) {
    SCHED_TRACE("Selecting next task...");

    // Check run queues from highest priority (0) downwards
    for (int prio = 0; prio < SCHED_PRIORITY_LEVELS; ++prio) {
        run_queue_t *queue = &g_run_queues[prio];

        // Quick check if queue might be empty (before locking)
        if (!queue->head) continue;

        // Lock this specific queue
        uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&queue->lock);

        tcb_t *task = queue->head;
        if (task) {
            // Found a task at the head of this priority queue.
            // Dequeue it.
            KERNEL_ASSERT(task->state == TASK_READY, "Task at head of run queue not READY");
            // Use the helper function which handles head/tail/count correctly
            bool dequeued = dequeue_task_locked(task); // dequeue requires the lock

            spinlock_release_irqrestore(&queue->lock, queue_irq_flags); // Release lock *after* dequeue

            if (!dequeued) {
                // This case should be rare if head wasn't NULL initially, but handle defensively
                SCHED_ERROR("Failed to dequeue task PID %lu from Prio %d queue after lock acquire!", task->pid, prio);
                continue; // Try next priority
            }

            // Reset its time slice based on its priority
            task->ticks_remaining = MS_TO_TICKS(g_priority_time_slices_ms[task->priority]);

            SCHED_DEBUG("Selected task PID %lu (Prio %d).", task->pid, prio);
            return task;
        } else {
             // Queue head was NULL after acquiring lock, release and continue
             spinlock_release_irqrestore(&queue->lock, queue_irq_flags);
        }
    }

    // No tasks found in any run queue, select the idle task
    SCHED_DEBUG("No ready tasks found, selecting idle task.");
    KERNEL_ASSERT(g_idle_task_tcb.state == TASK_READY || g_idle_task_tcb.state == TASK_RUNNING, "Idle task is not READY or RUNNING!");
    // Idle task is not explicitly dequeued/enqueued like others, it's the fallback.
    // Reset its "slice" conceptually, though it runs indefinitely when chosen.
    g_idle_task_tcb.ticks_remaining = g_idle_task_tcb.time_slice_ticks;
    return &g_idle_task_tcb;
}


/**
 * @brief Performs the low-level context switch or initial user jump.
 */
static void perform_context_switch(tcb_t *old_task, tcb_t *new_task) {
    KERNEL_ASSERT(new_task != NULL, "Cannot switch to a NULL task");
    KERNEL_ASSERT(new_task->process != NULL, "New task has NULL process");
    KERNEL_ASSERT(new_task->esp != NULL, "New task has NULL ESP");
    KERNEL_ASSERT(new_task->process->page_directory_phys != NULL, "New task has NULL page directory");

    g_context_switches++; // Increment counter for stats

    bool first_run = !new_task->has_run;

    // --- Update TSS ESP0 ---
    // The ESP0 field in the TSS should point to the *top* of the kernel stack
    // for the *incoming* task. This stack is used when transitioning from CPL3 to CPL0.
    uintptr_t new_kernel_stack_top;
    if (new_task->pid != IDLE_TASK_PID) {
        KERNEL_ASSERT(new_task->process->kernel_stack_vaddr_top != NULL, "Switch target has NULL kernel stack top");
        new_kernel_stack_top = (uintptr_t)new_task->process->kernel_stack_vaddr_top;
        SCHED_TRACE("Setting TSS ESP0 for switch to PID %lu: %p", new_task->pid, (void*)new_kernel_stack_top);
    } else {
        // Idle task uses its static stack buffer
        new_kernel_stack_top = PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);
        SCHED_TRACE("Setting TSS ESP0 for switch to Idle Task: %p", (void*)new_kernel_stack_top);
    }
    tss_set_kernel_stack((uint32_t)new_kernel_stack_top);


    // --- Prepare Switch Parameters ---
    uint32_t *new_pd_phys = new_task->process->page_directory_phys;
    uint32_t *new_esp = new_task->esp; // Kernel ESP to load (points into the task's kernel stack)
    uint32_t **old_task_esp_loc = old_task ? &(old_task->esp) : NULL; // Where to save old ESP

    if (new_task->pid != IDLE_TASK_PID && new_task->process && new_task->process->kernel_stack_vaddr_top) {
        uintptr_t kstack_top = (uintptr_t)new_task->process->kernel_stack_vaddr_top;
        // Check the page *containing* the top word, which is where ESP0 points.
        // ESP0 = 0xe0004000, points just past the end. The actual top page is 0xe0003000.
        uintptr_t kstack_page_to_check = PAGE_ALIGN_DOWN(kstack_top - 1); // Get page containing top byte
        uintptr_t kstack_phys_check = 0;
        // Check mapping in the *kernel's* page directory
        int kstack_map_status = paging_get_physical_address((uint32_t*)g_kernel_page_directory_phys,
                                                            kstack_page_to_check,
                                                            &kstack_phys_check);

        serial_write("[Sched Verify KStack] Checking V=");
        // Add hex print for kstack_page_to_check if possible
        serial_write(": ");
        if (kstack_map_status != 0 || kstack_phys_check == 0) {
            serial_write("!!! MAPPING INVALID/NOT PRESENT !!!\n");
            // Consider adding a loop or panic here if this occurs
        } else {
            serial_write("OK (Mapped to P=");
            // Add hex print for kstack_phys_check if possible
    
            serial_write(")\n");
        }
    }
    
    // Decide if page directory needs switching
    bool pd_needs_switch = (!old_task || !old_task->process || old_task->process->page_directory_phys != new_pd_phys);

    // --- Execute Switch/Jump ---
    if (first_run && new_task->pid != IDLE_TASK_PID) {
        // This is the very first time this user task is run.
        // We need to jump to user mode using the specially prepared initial stack.
        // The 'new_esp' currently points to the *kernel* stack frame prepared by create_process,
        // which contains the necessary registers for jump_to_user_mode (iret frame).
        new_task->has_run = true;
        SCHED_DEBUG("First run for PID %lu. Calling jump_to_user_mode(ESP=%p, PD=%p)",
                   new_task->pid, new_esp, new_pd_phys);
        serial_write("[Sched] Calling jump_to_user_mode...\n");
        // jump_to_user_mode switches page directory, loads user stack pointer, and iret's
        jump_to_user_mode(new_esp, new_pd_phys);
        // Should never return here!
        KERNEL_PANIC_HALT("jump_to_user_mode returned unexpectedly!");
    } else {
        // This is a regular context switch between kernel contexts (or to idle task).
        // 'old_task_esp_loc' is where context_switch will save the current kernel ESP.
        // 'new_esp' is the kernel ESP to load for the incoming task.
        // 'new_pd_phys' is the physical address of the new page directory (or NULL if no switch needed).
        if (first_run && new_task->pid == IDLE_TASK_PID) {
             new_task->has_run = true; // Mark idle task as run
        }
        SCHED_DEBUG("Kernel context switch: %lu -> %lu (PD Switch: %s, SaveESP@%p, LoadESP:%p)",
                   old_task ? old_task->pid : 0,
                   new_task->pid,
                   pd_needs_switch ? "YES" : "NO",
                   old_task_esp_loc, new_esp);

        serial_write("[Sched] Calling context_switch...\n");
        context_switch(old_task_esp_loc, new_esp, pd_needs_switch ? new_pd_phys : NULL);
        // Execution resumes HERE for the task being switched *back* to.
    }

    // Post-switch sanity check (optional)
    SCHED_TRACE("Resumed task PID %lu after context switch.", g_current_task ? g_current_task->pid : 0);
}

//============================================================================
// Scheduler Core Functions - DEFINITIONS START HERE
//============================================================================

/**
 * @brief Core scheduling function. Finds next task and switches context.
 * @note Can be called with interrupts disabled (e.g., from timer tick or yield).
 * Interrupts will be restored by the context switch mechanism (iret).
 */
void schedule(void) {
    SCHED_TRACE("Enter schedule()...");
    if (!g_scheduler_ready) {
        SCHED_TRACE("Scheduler not ready, exiting schedule().");
        return; // Not started yet
    }

    // Ensure interrupts are disabled on entry
    // (Should be true if called from timer tick, yield(), etc.)
    uint32_t eflags;
    asm volatile("pushf; pop %0" : "=r"(eflags));
    if (eflags & 0x200) {
        // KERNEL_PANIC_HALT("schedule() called with interrupts enabled!");
        // For safety, disable them if found enabled, but log it.
        SCHED_ERROR("schedule() called with interrupts enabled! Forcing disable.");
        asm volatile("cli");
    }


    // Acquire global lock, interrupts remain disabled
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);

    // Check sleep queue first (lock inside helper)
    // Acquire specific sleep queue lock
    uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
    check_sleeping_tasks_locked();
    spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);


    tcb_t *old_task = (tcb_t *)g_current_task; // Cast away volatile for local use
    tcb_t *new_task = scheduler_select_next_task(); // Selects and dequeues next READY task

    KERNEL_ASSERT(new_task != NULL, "scheduler_select_next_task returned NULL!");

    // If no switch is needed (selected task is current task)
    if (new_task == old_task) {
        SCHED_TRACE("Selected task PID %lu is same as current. No switch.", old_task ? old_task->pid : 0);

        // If the task was somehow marked READY (e.g., yield() then immediately chosen again),
        // ensure it's marked RUNNING.
        if (old_task && old_task->state == TASK_READY) {
             old_task->state = TASK_RUNNING;
        }

        // Ensure time slice is reset if it expired (select_next_task does this)
        // This might happen if the task yielded exactly when its slice ended.
        if (old_task && old_task->pid != IDLE_TASK_PID && old_task->ticks_remaining == 0) {
            old_task->ticks_remaining = old_task->time_slice_ticks;
             SCHED_TRACE("Reset time slice for PID %lu during no-switch", old_task->pid);
        }

        spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
        SCHED_TRACE("Exit schedule() - no switch.");
        // Restore interrupt flag if it was originally enabled?
        // No, context_switch / iret handles this. We should leave them disabled here.
        return;
    }

    // --- Task Switch Required ---
    SCHED_TRACE("Switching required: %lu -> %lu.", old_task ? old_task->pid : 0, new_task->pid);

    // 1. Update state of the outgoing task (if any)
    if (old_task) {
        // If it was running, put it back in the ready queue (unless blocked/zombie/sleeping)
        // The state change to SLEEPING or ZOMBIE happens *before* schedule() is called.
        if (old_task->state == TASK_RUNNING) {
             old_task->state = TASK_READY;
             // Re-enqueue at the tail of its priority queue
             uintptr_t old_queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[old_task->priority].lock);
             enqueue_task_locked(old_task);
             spinlock_release_irqrestore(&g_run_queues[old_task->priority].lock, old_queue_irq_flags);
             SCHED_TRACE("Re-enqueued old task PID %lu as READY.", old_task->pid);
        } else if (old_task->state == TASK_READY){
            // This case might happen if a task yielded and schedule() immediately
            // decided to switch away before the task ran again. It should already
            // be in the ready queue. No action needed here.
             SCHED_TRACE("Old task PID %lu was already READY.", old_task->pid);
        } else {
             // State is BLOCKED, SLEEPING, or ZOMBIE. Do not re-enqueue.
             SCHED_TRACE("Old task PID %lu state (%d) unchanged (not RUNNING or READY).", old_task->pid, old_task->state);
        }
    }

    // 2. Update state of the incoming task (already dequeued by select_next_task, unless it's idle)
    new_task->state = TASK_RUNNING;
    g_current_task = new_task; // Update global pointer **WHILE LOCK HELD**

    // 3. Release the GLOBAL scheduler lock *before* context switch
    // Interrupts are still disabled at this point.
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    // 4. Perform the low-level switch (handles TSS update, stack switch, page dir switch, iret/ret)
    // Interrupts are restored by perform_context_switch or its callees (specifically iret).
    perform_context_switch(old_task, new_task);

    // --- Resumption Point ---
    // Execution resumes here for the task being switched back TO.
    // Lock is NOT held. Interrupt state is restored by context_switch/iret.
    SCHED_TRACE("Returned to schedule() for PID %lu.", g_current_task ? g_current_task->pid : 0);
}

//============================================================================
// Public API Functions - DEFINITIONS START HERE
//============================================================================

// Note: scheduler_init definition is now at the end of the file

/**
 * @brief Creates a TCB for a process and adds it to the scheduler.
 */
int scheduler_add_task(pcb_t *pcb) {
    // --- Parameter Validation ---
    if (!pcb) return SCHED_ERR_INVALID;
    KERNEL_ASSERT(pcb->pid != IDLE_TASK_PID, "Attempting to add task with PID 0");
    KERNEL_ASSERT(pcb->page_directory_phys != NULL, "PCB Page Directory Phys is NULL");
    KERNEL_ASSERT(pcb->kernel_stack_vaddr_top != NULL, "PCB Kernel Stack Top is NULL");
    KERNEL_ASSERT(pcb->user_stack_top != NULL, "PCB User Stack Top is NULL");
    KERNEL_ASSERT(pcb->entry_point != 0, "PCB Entry Point is zero");
    KERNEL_ASSERT(pcb->kernel_esp_for_switch != 0, "PCB initial kernel ESP not set");

    SCHED_DEBUG("Request add task PID %lu (Entry=%p, KStackTop=%p, InitESP=%p)",
               pcb->pid, (void*)pcb->entry_point,
               pcb->kernel_stack_vaddr_top, (void*)pcb->kernel_esp_for_switch);

    // --- Allocate TCB ---
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        SCHED_ERROR("kmalloc failed for TCB (PID %lu).", pcb->pid);
        return SCHED_ERR_NOMEM;
    }
    memset(new_task, 0, sizeof(tcb_t));

    // --- Initialize TCB ---
    new_task->process = pcb;
    new_task->pid     = pcb->pid;
    new_task->state   = TASK_READY; // Start as ready
    new_task->has_run = false;     // Mark as not run yet
    new_task->esp     = (uint32_t*)pcb->kernel_esp_for_switch; // ESP prepared by create_process

    // Assign default priority and calculate initial time slice
    new_task->priority = SCHED_DEFAULT_PRIORITY;
    if (new_task->priority >= SCHED_PRIORITY_LEVELS) new_task->priority = SCHED_PRIORITY_LEVELS - 1; // Sanity
    new_task->time_slice_ticks = MS_TO_TICKS(g_priority_time_slices_ms[new_task->priority]);
    new_task->ticks_remaining = new_task->time_slice_ticks;
    new_task->runtime_ticks = 0;
    new_task->wakeup_time = 0;
    new_task->exit_code = 0;
    new_task->wait_next = NULL;
    new_task->wait_prev = NULL;
    new_task->wait_reason = NULL;
    new_task->all_tasks_next = NULL; // <<< ADDED: Initialize global list pointer

    // --- Add to Global Task List (Locked) --- <<< ADDED
    uintptr_t all_tasks_irq_flags = spinlock_acquire_irqsave(&g_all_tasks_lock);
    new_task->all_tasks_next = g_all_tasks_head;
    g_all_tasks_head = new_task;
    // Don't increment g_task_count here, do it when adding to run queue
    spinlock_release_irqrestore(&g_all_tasks_lock, all_tasks_irq_flags);


    // --- Add to Scheduler Run Queue (Locked) ---
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[new_task->priority].lock);

    enqueue_task_locked(new_task);
    g_task_count++; // Increment total *active* task count now

    spinlock_release_irqrestore(&g_run_queues[new_task->priority].lock, queue_irq_flags);
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    // Corrected format specifiers for warnings
    SCHED_INFO("Added task PID %lu (Prio %u, Slice %lu ticks). Total active tasks: %lu",
              new_task->pid, (unsigned int)new_task->priority, (unsigned long)new_task->time_slice_ticks, g_task_count);
    return SCHED_OK;
}

/** @brief Voluntarily yields the CPU. */
void yield(void) {
    SCHED_TRACE("Task PID %lu yielding.", g_current_task ? g_current_task->pid : 0);
    uint32_t eflags_save;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags_save)); // Disable interrupts, save previous state

    // schedule() expects interrupts to be disabled
    schedule(); // Trigger scheduler

    // Interrupts will be restored by iret in the context switch mechanism.
    // No explicit sti needed here. If schedule() didn't switch, interrupts remain disabled.
    // This is generally fine as the task will likely continue and re-enable them soon,
    // or another interrupt/tick will occur. If an explicit sti is needed here,
    // it implies a flaw in the schedule() no-switch path or context switch restore logic.

    // if (eflags_save & 0x200) { asm volatile("sti"); } // Generally NOT needed here

    SCHED_TRACE("Task PID %lu resumed after yield.", g_current_task ? g_current_task->pid : 0);
}


/** @brief Puts the current task to sleep for a specified duration. */
void sleep_ms(uint32_t ms) {
      if (ms == 0) {
           yield(); // Sleep 0 means yield
           return;
      }

      // Disable interrupts for critical section
      uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);

      tcb_t *current = (tcb_t*)g_current_task; // Cast away volatile
      KERNEL_ASSERT(current != NULL, "sleep_ms called with NULL current task");
      KERNEL_ASSERT(current->pid != IDLE_TASK_PID, "Idle task cannot sleep");
      // Task might yield then immediately call sleep, could be READY momentarily
      KERNEL_ASSERT(current->state == TASK_RUNNING || current->state == TASK_READY, "Task not RUNNING/READY cannot sleep");

      // Calculate wakeup time (handle potential tick wrap around)
      uint32_t current_ticks = g_tick_count;
      uint32_t ticks_to_wait = MS_TO_TICKS(ms);
      if (ticks_to_wait == 0 && ms > 0) ticks_to_wait = 1; // Minimum 1 tick sleep
      // Avoid excessively long sleeps causing wrap issues before expected time
      if (ticks_to_wait > (UINT32_MAX / 2)) ticks_to_wait = UINT32_MAX / 2;

      current->wakeup_time = current_ticks + ticks_to_wait;
      // Simple wrap check (less accurate for very long sleeps near wrap)
      if (current->wakeup_time < current_ticks) {
           SCHED_DEBUG("Sleep for PID %lu wraps tick counter (Cur: %lu, Wait: %lu, Wake: %lu).",
                current->pid, current_ticks, ticks_to_wait, current->wakeup_time);
      } else {
           SCHED_DEBUG("Task PID %lu sleeping for %lu ms (%lu ticks) until tick %lu.",
                current->pid, (unsigned long)ms, (unsigned long)ticks_to_wait, (unsigned long)current->wakeup_time);
      }

      // --- Critical Section for State Change & Queue Manipulation ---
      // Mark as sleeping *before* queue manipulation
      current->state = TASK_SLEEPING;

      // The task is currently RUNNING (or maybe READY if interrupted just right),
      // so it's *not* in the run queue. We just need to add it to the sleep queue.
      // schedule() will handle not re-adding it to the run queue because its state is SLEEPING.

      // Add to sleep queue (must lock sleep queue)
      uintptr_t sleep_irq_flags = spinlock_acquire_irqsave(&g_sleep_queue.lock);
      add_to_sleep_queue_locked(current);
      spinlock_release_irqrestore(&g_sleep_queue.lock, sleep_irq_flags);
      // --- End Critical Section ---


      // Release global lock *before* scheduling
      spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

      // Trigger schedule to switch away from the now-sleeping task
      // schedule() expects interrupts disabled.
      asm volatile ("cli");
      schedule();

      // Execution resumes here when woken up by scheduler_tick -> check_sleeping_tasks
      // Interrupts should be restored by the context switch mechanism.
      SCHED_TRACE("Task PID %lu resumed after sleep.", current ? current->pid : 0); // Re-check current as it might have changed
}


/** @brief Marks the current task as ZOMBIE and triggers scheduler. */
void remove_current_task_with_code(uint32_t code) {
    asm volatile ("cli"); // Ensure interrupts off

    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);

    tcb_t *task_to_terminate = (tcb_t *)g_current_task; // Cast volatile away

    KERNEL_ASSERT(task_to_terminate != NULL, "remove_current_task called with NULL current task!");
    KERNEL_ASSERT(task_to_terminate->pid != IDLE_TASK_PID, "Attempting to remove the idle task!");
    // Task could be RUNNING or potentially BLOCKED/READY if termination happens asynchronously
    KERNEL_ASSERT(task_to_terminate->state == TASK_RUNNING ||
                  task_to_terminate->state == TASK_READY ||
                  task_to_terminate->state == TASK_BLOCKED,
                  "Task being removed is not RUNNING/READY/BLOCKED!");

    SCHED_INFO("Task PID %lu exiting with code %lu. Marking as ZOMBIE.",
              task_to_terminate->pid, (unsigned long)code);

    // Change state FIRST
    task_to_terminate->state = TASK_ZOMBIE;
    task_to_terminate->exit_code = code;

    // If it was blocked, remove from wait queue (wait queue logic needed separately)
    // if (task_to_terminate->state == TASK_BLOCKED) {
    //   // acquire wait queue lock
    //   // remove_from_wait_queue_locked(task_to_terminate);
    //   // release wait queue lock
    // }

    // Task is no longer runnable.
    // DO NOT decrement g_task_count here. Task still exists in the global list
    // as a zombie until cleaned up by scheduler_cleanup_zombies().
    // g_task_count--; // <<< MODIFIED: Removed this line

    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);

    // Call schedule() to switch away. Interrupts stay off until next task runs.
    schedule();

    // Should not return here
    KERNEL_PANIC_HALT("Returned from schedule() in remove_current_task!");
}


/** @brief Returns a volatile pointer to the currently running task's TCB. */
volatile tcb_t *get_current_task_volatile(void) {
    return g_current_task;
}

/** @brief Returns a non-volatile pointer to the currently running task's TCB. */
tcb_t *get_current_task(void) {
    // Read volatile pointer once
    volatile tcb_t* current_v = g_current_task;
    return (tcb_t *)current_v;
}

/** @brief Retrieves basic scheduler statistics. */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    if (out_task_count) *out_task_count = g_task_count; // Count reflects active (non-zombie) tasks
    if (out_switches) *out_switches = g_context_switches;
    spinlock_release_irqrestore(&g_scheduler_global_lock, irq_flags);
}

/** @brief Checks if the scheduler is active. */
bool scheduler_is_ready(void) {
    return g_scheduler_ready;
}

/** @brief Marks the scheduler as ready to perform context switching. */
void scheduler_start(void) {
    SCHED_INFO("Scheduler marked as ready. Preemption will begin on next schedule() call.");
    g_scheduler_ready = true;
    // Usually, the first context switch happens here, often explicitly switching to idle or the first real task.
    // For simplicity, we let the next timer tick or yield trigger the first schedule().
    // Alternatively, call schedule() here after enabling interrupts:
    // asm volatile ("sti"); schedule();
}

/** @brief Debugging function to dump current scheduler state. */
static void dump_scheduler_state(void) {
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);

    terminal_printf("\n--- Scheduler State Dump ---\n");
    terminal_printf("Current Tick: %lu\n", g_tick_count);
    terminal_printf("Active Tasks: %lu\n", g_task_count); // Count reflects active tasks
    terminal_printf("Context Switches: %lu\n", g_context_switches);
    tcb_t* current = (tcb_t*)g_current_task; // Cast volatile away for inspection
    terminal_printf("Current Task: PID %lu (State: %d, Prio: %u, Ticks Left: %lu)\n",
        current ? current->pid : 0,
        current ? current->state : (task_state_e)-1,
        current ? current->priority : 0,
        current ? (unsigned long)current->ticks_remaining : 0);

    for (int i=0; i < SCHED_PRIORITY_LEVELS; ++i) {
        run_queue_t* q = &g_run_queues[i];
        uintptr_t q_irq = spinlock_acquire_irqsave(&q->lock);
        terminal_printf(" Run Queue Prio %d (Count: %lu): ", i, (unsigned long)q->count);
        tcb_t *task = q->head;
        while(task) {
            terminal_printf("%lu(%d) -> ", task->pid, task->state);
            task = task->next;
        }
        terminal_write("NULL\n");
        spinlock_release_irqrestore(&q->lock, q_irq);
    }

     uintptr_t s_irq = spinlock_acquire_irqsave(&g_sleep_queue.lock);
     terminal_printf(" Sleep Queue (Count: %lu): ", (unsigned long)g_sleep_queue.count);
     tcb_t *stask = g_sleep_queue.head;
     while(stask) {
         terminal_printf("%lu(W:%lu) -> ", stask->pid, (unsigned long)stask->wakeup_time);
         stask = stask->wait_next;
     }
     terminal_write("NULL\n");
     spinlock_release_irqrestore(&g_sleep_queue.lock, s_irq);

     // Dump global task list (optional, could be long)
     uintptr_t a_irq = spinlock_acquire_irqsave(&g_all_tasks_lock);
     terminal_printf(" Global Task List Head: %p\n", g_all_tasks_head);
     // Add iteration here if needed for deep debug
     spinlock_release_irqrestore(&g_all_tasks_lock, a_irq);


    terminal_write("--------------------------\n");
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);
}

/**
 * @brief Initializes the advanced priority-based scheduler subsystem.
 * @details Sets up priority run queues, sleep queue, locks, idle task,
 * global task list, and initial TSS ESP0 value.
 */
void scheduler_init(void) {
    SCHED_INFO("Initializing advanced priority scheduler...");

    // Initialize global scheduler state variables
    g_current_task = NULL;
    g_task_count = 0; // Count starts at 0, idle task increments it later
    g_context_switches = 0;
    g_tick_count = 0; // Initialize global tick count
    g_scheduler_ready = false; // Scheduler is not ready until started
    g_all_tasks_head = NULL; // Initialize global list

    // Initialize locks
    spinlock_init(&g_scheduler_global_lock);
    spinlock_init(&g_all_tasks_lock); // <<< ADDED: Initialize global list lock

    // Initialize run queues for each priority level
    SCHED_DEBUG("Initializing %d priority run queues...", SCHED_PRIORITY_LEVELS);
    for (int i = 0; i < SCHED_PRIORITY_LEVELS; i++) {
        init_run_queue(&g_run_queues[i]);
    }

    // Initialize sleep queue
    SCHED_DEBUG("Initializing sleep queue...");
    init_sleep_queue();

    // Initialize the mandatory idle task's TCB, PCB, and add to global list
    // Note: This must happen before adding it to the run queue.
    scheduler_init_idle_task(); // Sets up TCB/PCB, initial ESP, adds to g_all_tasks_head

    // Add idle task to its appropriate run queue (must happen after init)
    // Requires locks as it modifies shared queue state.
    uintptr_t global_irq_flags = spinlock_acquire_irqsave(&g_scheduler_global_lock);
    uintptr_t queue_irq_flags = spinlock_acquire_irqsave(&g_run_queues[g_idle_task_tcb.priority].lock);

    // Enqueue idle task (it's already marked READY)
    enqueue_task_locked(&g_idle_task_tcb);
    g_task_count++; // Increment total *active* task count for the idle task

    spinlock_release_irqrestore(&g_run_queues[g_idle_task_tcb.priority].lock, queue_irq_flags);
    spinlock_release_irqrestore(&g_scheduler_global_lock, global_irq_flags);


    // Set the initial TSS ESP0 value to the idle task's kernel stack top
    // This is crucial for handling the first interrupt/syscall after boot before any task switch.
    uintptr_t idle_stack_virt_top = PHYS_TO_VIRT((uintptr_t)g_idle_task_pcb.kernel_stack_vaddr_top);
    SCHED_DEBUG("Setting initial TSS ESP0 to idle task stack top: %p", (void*)idle_stack_virt_top);
    tss_set_kernel_stack((uint32_t)idle_stack_virt_top); // Update TSS

    // <<< ADD THIS LINE >>>
    // Explicitly set the initial current task to the idle task.
    // schedule() will confirm this on the first run.
    g_current_task = &g_idle_task_tcb;
    SCHED_DEBUG("Set initial g_current_task = %p (Idle Task)", g_current_task);
    // <<< END ADDED LINE >>>


    SCHED_INFO("Priority scheduler initialized (Idle Task PID %lu created, Prio %u). Active tasks: %lu",
        (unsigned long)IDLE_TASK_PID, (unsigned int)g_idle_task_tcb.priority, g_task_count);
    SCHED_INFO("Time slices (ms): P0=%lu, P1=%lu, P2=%lu, P3=%lu",
        (unsigned long)g_priority_time_slices_ms[0],
        (unsigned long)g_priority_time_slices_ms[1],
        (unsigned long)g_priority_time_slices_ms[2],
        (unsigned long)g_priority_time_slices_ms[3]);

    // Scheduler is initialized but not 'ready' for switching until scheduler_start() is called.
}