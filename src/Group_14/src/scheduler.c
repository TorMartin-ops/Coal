/**
 * scheduler.c - Production-Quality Kernel Scheduler Implementation (Revised)
 *
 * Author: Group 14 (UiA) & Gemini
 * Version: 3.4
 *
 * Features:
 * - Task States: READY, RUNNING, BLOCKED, ZOMBIE for better management.
 * - Locking: Uses spinlocks for SMP safety on scheduler data structures.
 * - Robustness: Added checks for NULL pointers and invalid states via KERNEL_ASSERT.
 * - Task Termination: Implements a safer termination mechanism (zombie state)
 * deferring cleanup, avoiding context switch issues during removal.
 * - Clarity: Improved structure, comments, variable names, and logging formats.
 * - Debugging: Includes assertions and targeted logging for critical paths.
 * - Idle Task: Includes handling for a dedicated idle task.
 *
 * Fixes:
 * - Removed definition of pit_set_scheduler_ready (should be in pit.c).
 * - Corrected printf format specifiers for various types (uint32_t, uintptr_t).
 * - Replaced ENOMEM with literal error code.
 * - Removed unused 'switch_pd' variable.
 * - Corrected conflicting type for debug_scheduler_stats (reverted counter to uint32_t).
 */

// === Standard/Core Headers ===
#include <libc/stdint.h>    // Fixed-width integers (uint32_t, uintptr_t, etc.)
#include <libc/stddef.h>    // For NULL, size_t
#include <libc/stdbool.h>   // For bool type
#include <string.h>         // Kernel's string functions (memset) - Ensure this is your kernel's string.h

// === Kernel Subsystems & Drivers ===
#include "scheduler.h"      // For tcb_t, pcb_t, function prototypes, task_state_t
#include "process.h"        // For pcb_t, destroy_process, PROCESS_KSTACK_SIZE
#include "kmalloc.h"        // For kmalloc/kfree
#include "terminal.h"       // For terminal_printf, terminal_write, etc.
#include "spinlock.h"       // Spinlock implementation
#include "idt.h"            // For interrupt enable/disable macros or placeholders (like irqsave flags)
#include "gdt.h"            // For GDT selectors (USER_CODE_SELECTOR, etc.)
#include "assert.h"         // For KERNEL_ASSERT macro
#include "pit.h"            // For pit_set_scheduler_ready declaration (assumed)

// --- GDT Selectors (Ensure these match your GDT setup in gdt.c/gdt.h) ---
// Using definitions from gdt.h is preferred if available
#ifndef GDT_USER_CODE_SELECTOR
#define GDT_USER_CODE_SELECTOR   0x1B  // Example: GDT Index 3 + RPL 3
#endif
#ifndef GDT_USER_DATA_SELECTOR
#define GDT_USER_DATA_SELECTOR   0x23  // Example: GDT Index 4 + RPL 3
#endif
#ifndef GDT_KERNEL_CODE_SELECTOR
#define GDT_KERNEL_CODE_SELECTOR 0x08  // Example: GDT Index 1
#endif
#ifndef GDT_KERNEL_DATA_SELECTOR
#define GDT_KERNEL_DATA_SELECTOR 0x10  // Example: GDT Index 2
#endif

// --- Debug Logging Macros ---
// Using %lu for uint32_t (long unsigned int), %p for pointers/addresses (uintptr_t)
// Use %lx for hex representation of uint32_t if needed.
#define SCHED_LOG(fmt, ...) terminal_printf("[Scheduler] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) terminal_printf("[Scheduler Debug] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) terminal_printf("[Scheduler ERROR] " fmt "\n", ##__VA_ARGS__)

// --- External Assembly Functions ---
// Context switch function - defined in context_switch.asm
extern void context_switch(uint32_t **old_esp_ptr,
                           uint32_t *new_esp,
                           uint32_t *new_page_directory_phys);

// PIT notification function - defined in pit.c
// Declared here assuming it's in pit.h which should be included.
extern void pit_set_scheduler_ready(void);

// --- Static Globals ---
static tcb_t          *task_list_head   = NULL;    // Head of the circular list of ALL tasks
static volatile tcb_t *current_task    = NULL;    // The currently RUNNING task (must be volatile)
static uint32_t        task_count       = 0;       // Total tasks (any state)
static uint32_t        context_switches = 0;       // Count switches (reverted to uint32_t)
static spinlock_t      scheduler_lock;             // Protects task list, current_task, counts
static volatile bool   scheduler_ready  = false;   // Set by pit_set_scheduler_ready() when PIT is configured

// --- Idle Task ---
static tcb_t idle_task_tcb;
static pcb_t idle_task_pcb; // Minimal PCB for the idle task
#define IDLE_TASK_PID 0 // Assign PID 0 to idle task

// --- Forward Declarations ---
static tcb_t* select_next_task(void);
static void kernel_idle_task_loop(void); // The actual function the idle task runs

/**
 * @brief The main loop for the kernel's idle task.
 */
static void kernel_idle_task_loop(void) {
    SCHED_LOG("Idle task started. CPU PID: %u", (unsigned long)IDLE_TASK_PID);
    while(1) {
        // TODO: Implement zombie task cleanup here periodically if desired.
        // scheduler_cleanup_zombies();

        asm volatile("sti");
        asm volatile("hlt");
    }
}

/**
 * @brief Initializes the dedicated idle task TCB and PCB.
 */
static void scheduler_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");

    // --- Setup Minimal PCB for Idle Task ---
    memset(&idle_task_pcb, 0, sizeof(pcb_t));
    idle_task_pcb.pid = IDLE_TASK_PID;
    idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys; // Uses kernel PD
    idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop;

    // --- Allocate Kernel Stack for Idle Task ---
    static uint8_t idle_stack[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
    idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)((uintptr_t)idle_stack + sizeof(idle_stack));
    idle_task_pcb.user_stack_top = NULL;

    // --- Setup TCB for Idle Task ---
    memset(&idle_task_tcb, 0, sizeof(tcb_t));
    idle_task_tcb.process = &idle_task_pcb;
    idle_task_tcb.pid = IDLE_TASK_PID;
    idle_task_tcb.state = TASK_READY;

    // --- Prepare Initial Stack Pointer for Idle Task ---
    uint32_t *kstack_ptr = (uint32_t *)idle_task_pcb.kernel_stack_vaddr_top;

    // Context switch frame (must match pops in context_switch.asm)
    *(--kstack_ptr) = (uint32_t)idle_task_pcb.entry_point; // EIP
    *(--kstack_ptr) = 0; // EAX
    *(--kstack_ptr) = 0; // ECX
    *(--kstack_ptr) = 0; // EDX
    *(--kstack_ptr) = 0; // EBX
    *(--kstack_ptr) = 0; // ESP_ignore
    *(--kstack_ptr) = 0; // EBP
    *(--kstack_ptr) = 0; // ESI
    *(--kstack_ptr) = 0; // EDI
    *(--kstack_ptr) = 0x00000202; // EFLAGS (IF=1)
    *(--kstack_ptr) = GDT_KERNEL_DATA_SELECTOR; // DS
    *(--kstack_ptr) = GDT_KERNEL_DATA_SELECTOR; // ES
    *(--kstack_ptr) = GDT_KERNEL_DATA_SELECTOR; // FS
    *(--kstack_ptr) = GDT_KERNEL_DATA_SELECTOR; // GS

    idle_task_tcb.esp = kstack_ptr;

    SCHED_DEBUG("Idle task initialized: ESP=%p", idle_task_tcb.esp);
}


/**
 * @brief Initialize the scheduler structures.
 */
void scheduler_init(void)
{
    task_list_head   = NULL;
    current_task     = NULL;
    task_count       = 0;
    context_switches = 0;
    scheduler_ready  = false;
    spinlock_init(&scheduler_lock);

    scheduler_init_idle_task();

    // Add idle task to the list
    task_list_head = &idle_task_tcb;
    idle_task_tcb.next = &idle_task_tcb;
    task_count++;

    SCHED_LOG("Scheduler Initialized (Idle Task PID %u created).", (unsigned long)IDLE_TASK_PID);
}

/**
 * @brief Create a TCB around the given PCB and insert into the task list.
 * @param pcb Pointer to a valid pcb_t structure.
 * @return 0 on success, negative on error.
 */
int scheduler_add_task(pcb_t *pcb)
{
    // --- Parameter Validation ---
    KERNEL_ASSERT(pcb != NULL, "NULL PCB passed to scheduler_add_task");
    KERNEL_ASSERT(pcb->pid != IDLE_TASK_PID, "Attempting to add task with reserved IDLE_TASK_PID");
    KERNEL_ASSERT(pcb->page_directory_phys != NULL, "PCB has NULL page directory");
    KERNEL_ASSERT(pcb->kernel_stack_vaddr_top != NULL, "PCB has NULL kernel stack top");
    KERNEL_ASSERT(pcb->user_stack_top != NULL, "PCB has NULL user stack top");
    KERNEL_ASSERT(pcb->entry_point != 0, "PCB has zero entry point");
    KERNEL_ASSERT((uintptr_t)pcb->kernel_stack_vaddr_top > PROCESS_KSTACK_SIZE, "Kernel stack top seems invalid");
    KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 256, "Kernel stack size too small");

    // Use %lu for uint32_t PID, %p for pointers
    SCHED_DEBUG("Adding task PID %u Entry=%p UserStackTop=%p KernelStackTop=%p",
                (unsigned long)pcb->pid, (void*)pcb->entry_point,
                pcb->user_stack_top, pcb->kernel_stack_vaddr_top);

    // --- Allocate TCB ---
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        // Use %lu for uint32_t PID
        SCHED_ERROR("Out of memory for TCB (PID %u).", (unsigned long)pcb->pid);
        return -1; // Simple error code, no ENOMEM
    }
    memset(new_task, 0, sizeof(tcb_t));

    new_task->process = pcb;
    new_task->pid     = pcb->pid;
    new_task->state   = TASK_READY;

    // --- Set up initial kernel stack frame for FIRST kernel-to-user mode switch (via IRET) ---
    uint32_t *kstack_ptr = (uint32_t *)pcb->kernel_stack_vaddr_top;
    uintptr_t kstack_base = (uintptr_t)pcb->kernel_stack_vaddr_top - PROCESS_KSTACK_SIZE;

    // Use %lu for uint32_t PID, %p for pointers
    SCHED_DEBUG("Preparing initial stack frame for PID %u at KStackTop=%p (Base=%p)",
                (unsigned long)pcb->pid, kstack_ptr, (void*)kstack_base);

    // --- IRET Frame ---
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR;
    *(--kstack_ptr) = (uintptr_t)pcb->user_stack_top;
    *(--kstack_ptr) = 0x00000202; // EFLAGS (IF=1)
    *(--kstack_ptr) = GDT_USER_CODE_SELECTOR;
    *(--kstack_ptr) = (uintptr_t)pcb->entry_point;

    // --- Context Switch Frame ---
    *(--kstack_ptr) = 0; // EAX
    *(--kstack_ptr) = 0; // ECX
    *(--kstack_ptr) = 0; // EDX
    *(--kstack_ptr) = 0; // EBX
    *(--kstack_ptr) = 0; // ESP_ignore
    *(--kstack_ptr) = 0; // EBP
    *(--kstack_ptr) = 0; // ESI
    *(--kstack_ptr) = 0; // EDI
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR; // DS
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR; // ES
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR; // FS
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR; // GS

    KERNEL_ASSERT((uintptr_t)kstack_ptr > kstack_base, "Kernel stack underflow");

    new_task->esp = kstack_ptr;

    // --- Insert TCB into the circular linked list ---
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);
    KERNEL_ASSERT(task_list_head != NULL, "Task list head NULL");
    new_task->next = task_list_head->next;
    task_list_head->next = new_task;
    task_count++;
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    // Use %lu for uint32_t PID, %p for pointers
    SCHED_LOG("Added task PID %u (KStackTop=%p, Init ESP=%p)",
              (unsigned long)pcb->pid,
              pcb->kernel_stack_vaddr_top,
              new_task->esp);

    return 0; // success
}

/**
 * @brief Voluntarily give up the CPU.
 */
void yield(void)
{
    uint32_t eflags = 0;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));
    schedule();
    if (eflags & 0x200) { asm volatile("sti"); }
}

/**
 * @brief Finds the next READY task to run.
 * @return Pointer to the next TCB or idle task TCB.
 * @note Must be called with scheduler_lock held.
 */
static tcb_t* select_next_task(void)
{
    KERNEL_ASSERT(task_list_head != NULL, "select_next_task: empty list");

    tcb_t *candidate = current_task ? current_task->next : task_list_head;
    tcb_t *starting_point = candidate;

    do {
        KERNEL_ASSERT(candidate != NULL, "NULL task in list");
        if (candidate->pid != IDLE_TASK_PID && candidate->state == TASK_READY) {
            return candidate;
        }
        candidate = candidate->next;
    } while (candidate != starting_point);

    KERNEL_ASSERT(idle_task_tcb.state == TASK_READY, "Idle task not ready!");
    return &idle_task_tcb;
}

/**
 * @brief The core scheduler function. Finds and switches to a new task.
 * @note Should be called with interrupts disabled.
 */
void schedule(void)
{
    if (!scheduler_ready) return;

    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    tcb_t *old_task = (tcb_t *)current_task;
    tcb_t *new_task = select_next_task();

    KERNEL_ASSERT(new_task != NULL, "select_next_task NULL!");

    if (new_task == old_task) {
        if (current_task && current_task->state != TASK_RUNNING) {
             KERNEL_ASSERT(current_task->state == TASK_READY, "Current task not RUNNING or READY");
             current_task->state = TASK_RUNNING;
        }
        spinlock_release_irqrestore(&scheduler_lock, irq_flags);
        return;
    }

    context_switches++;

    if (old_task) {
        if (old_task->state == TASK_RUNNING) {
            old_task->state = TASK_READY;
        }
        KERNEL_ASSERT(old_task->state == TASK_READY || old_task->state == TASK_BLOCKED || old_task->state == TASK_ZOMBIE,
                      "Old task unexpected state");
    }

    KERNEL_ASSERT(new_task->state == TASK_READY, "Selected next task not READY?");
    new_task->state = TASK_RUNNING;
    current_task = new_task;

    uint32_t *new_pd_phys = NULL;
    if (!old_task || old_task->process != new_task->process) {
         KERNEL_ASSERT(new_task->process != NULL, "New task NULL process!");
         KERNEL_ASSERT(new_task->process->page_directory_phys != NULL, "New task NULL PD!");
         new_pd_phys = new_task->process->page_directory_phys;
         // Removed unused 'switch_pd' variable
    }

    uint32_t **old_task_esp_loc = old_task ? &(old_task->esp) : NULL;

    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    context_switch(old_task_esp_loc, new_task->esp, new_pd_phys);
}

/**
 * @brief Returns a volatile pointer to the currently running task's TCB.
 */
volatile tcb_t *get_current_task_volatile(void)
{
    return current_task;
}

/**
 * @brief Returns a non-volatile pointer to the currently running task's TCB.
 * @warning Use with caution when interrupts are enabled.
 */
tcb_t *get_current_task(void)
{
    return (tcb_t *)current_task;
}

/**
 * @brief Marks the current running task as ZOMBIE and triggers a context switch.
 * @param code Exit code (for logging).
 * @note Must be called with interrupts disabled. Does not return.
 */
void remove_current_task_with_code(uint32_t code)
{
    asm volatile("cli"); // Ensure interrupts off

    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    tcb_t *task_to_terminate = (tcb_t *)current_task;

    KERNEL_ASSERT(task_to_terminate != NULL, "remove_current_task: current_task NULL!");
    KERNEL_ASSERT(task_to_terminate->state == TASK_RUNNING || task_to_terminate->state == TASK_READY || task_to_terminate->state == TASK_BLOCKED,
                  "Task being removed not RUNNING/READY/BLOCKED!");
    KERNEL_ASSERT(task_to_terminate->pid != IDLE_TASK_PID, "Attempting to remove idle task!");

    // Use %lu for uint32_t PID and code
    SCHED_LOG("Task PID %u exiting with code %u. Marking as ZOMBIE.",
              (unsigned long)task_to_terminate->pid, (unsigned long)code);

    task_to_terminate->state = TASK_ZOMBIE;

    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    schedule();

    KERNEL_PANIC_HALT("Returned after schedule() in remove_current_task!");
}

/**
 * @brief Reap and free any tasks marked as ZOMBIE.
 */
void scheduler_cleanup_zombies(void)
{
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    if (!task_list_head || task_count <= 1) {
        spinlock_release_irqrestore(&scheduler_lock, irq_flags);
        return;
    }

    tcb_t *prev = task_list_head;
    tcb_t *current = task_list_head->next;

    while (current != task_list_head) {
        KERNEL_ASSERT(current != NULL, "NULL task in zombie cleanup");
        tcb_t *next_task = current->next;

        if (current->state == TASK_ZOMBIE) {
            // Use %lu for uint32_t PID
            SCHED_LOG("Cleanup: Reaping ZOMBIE task PID %u.", (unsigned long)current->pid);

            prev->next = next_task;
            task_count--;
            KERNEL_ASSERT(task_count > 0, "Task count zero during zombie cleanup?");

            pcb_t* pcb_to_free = current->process;
            tcb_t* tcb_to_free = current;

            spinlock_release_irqrestore(&scheduler_lock, irq_flags);

            if (pcb_to_free) { destroy_process(pcb_to_free); }
            kfree(tcb_to_free);

            irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

            if (!task_list_head || task_count <= 1 || task_list_head->next == task_list_head) {
                break;
            }
            current = next_task; // Continue scan from the node after the removed one
        } else {
            prev = current;
            current = next_task;
        }
        if (current == NULL) {
             SCHED_ERROR("NULL pointer encountered during zombie cleanup scan!");
             break;
        }
    }
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);
}


/**
 * @brief Retrieve basic scheduler statistics.
 * @param out_task_count: If non-NULL, stores current task count.
 * @param out_switches:   If non-NULL, stores context switch count.
 */
// Changed signature back to uint32_t for out_switches to match header
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches)
{
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);
    if (out_task_count) {
        *out_task_count = task_count;
    }
    if (out_switches) {
        // context_switches is uint32_t now
        *out_switches = context_switches;
    }
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);
}


/**
 * @brief Returns the ready state of the scheduler.
 */
bool scheduler_is_ready(void)
{
    return scheduler_ready;
}

// Definition of pit_set_scheduler_ready() removed.
// It should be defined in pit.c

