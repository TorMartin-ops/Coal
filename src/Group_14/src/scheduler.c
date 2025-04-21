/**
 * scheduler.c - Production-Quality Kernel Scheduler Implementation (Revised)
 *
 * Author: Group 14 (UiA) & Gemini
 * Version: 3.9 (Build fixes for prototypes and types)
 *
 * Implements a simple round-robin preemptive scheduler.
 * Handles task creation, termination (via ZOMBIE state), context switching,
 * and includes a dedicated idle task. Differentiates initial user-mode
 * launch (via IRET) from subsequent kernel-to-kernel switches.
 */

// === Standard/Core Headers ===
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>
#include <string.h> // Include your kernel's string header

// === Kernel Subsystems & Drivers ===
#include "scheduler.h"      // Public interface (now includes asm prototypes)
#include "process.h"        // For pcb_t, destroy_process, PROCESS_KSTACK_SIZE
#include "kmalloc.h"        // For kmalloc/kfree
#include "terminal.h"       // For terminal_printf, terminal_write
#include "spinlock.h"       // Spinlock implementation
#include "idt.h"            // For interrupt control (saving/restoring flags)
#include "gdt.h"            // For GDT selectors
#include "assert.h"         // For KERNEL_ASSERT, KERNEL_PANIC_HALT
// #include "pit.h"         // No longer directly needed for scheduler readiness
#include "paging.h"         // For g_kernel_page_directory_phys (used by idle task)
#include "tss.h"            // For tss_set_kernel_stack, TSS_SELECTOR

// --- Kernel Error Codes (Example) ---
// Define these properly in a central error header if possible
#define SCHED_OK         0
#define SCHED_ERR_NOMEM -1 // Out of memory
#define SCHED_ERR_FAIL  -2 // General failure



// --- Debug Logging Macros ---
// Using %lu for uint32_t, %p for pointers, %lx for hex
#define SCHED_LOG(fmt, ...) terminal_printf("[Scheduler] " fmt "\n", ##__VA_ARGS__)
#define SCHED_DEBUG(fmt, ...) terminal_printf("[Scheduler Debug] " fmt "\n", ##__VA_ARGS__)
#define SCHED_ERROR(fmt, ...) terminal_printf("[Scheduler ERROR] " fmt "\n", ##__VA_ARGS__)

// --- Static Globals ---
static tcb_t          *task_list_head   = NULL;    // Head of the circular list of ALL tasks
static volatile tcb_t *current_task    = NULL;    // The currently RUNNING task (volatile)
static uint32_t        task_count       = 0;       // Total tasks (any state)
static uint32_t        context_switches = 0;       // Context switch counter
static spinlock_t      scheduler_lock;             // Protects scheduler shared data

// --- Global Scheduler Ready Flag (Defined Here) ---
volatile bool g_scheduler_ready = false;

// --- Idle Task Data ---
static tcb_t idle_task_tcb;
static pcb_t idle_task_pcb; // Minimal PCB for the idle task

// --- Forward Declarations ---
static tcb_t* select_next_task(void);
static void kernel_idle_task_loop(void) __attribute__((noreturn)); // Mark as noreturn
static void scheduler_init_idle_task(void);

//----------------------------------------------------------------------------
// Idle Task Implementation
//----------------------------------------------------------------------------

/**
 * @brief The main loop for the kernel's idle task. Runs when no other task is ready.
 * Executes HLT instruction repeatedly, waking only on interrupts.
 */
static void kernel_idle_task_loop(void) {
    SCHED_LOG("Idle task started. PID: %lu", (unsigned long)IDLE_TASK_PID);
    while(1) {
        // Periodically attempt to clean up finished (zombie) tasks.
        // Consider doing this less frequently if it impacts performance/power.
        scheduler_cleanup_zombies();

        // Enable interrupts and halt until the next interrupt occurs.
        asm volatile("sti; hlt");

        // Interrupts are disabled automatically by the CPU upon entering an ISR.
        // The loop continues after the interrupt handler returns.
    }
}

/**
 * @brief Initializes the TCB and minimal PCB for the dedicated idle task.
 * Sets up the initial kernel stack context for the idle task loop.
 */
static void scheduler_init_idle_task(void) {
    SCHED_DEBUG("Initializing idle task...");

    // --- Setup Minimal PCB ---
    memset(&idle_task_pcb, 0, sizeof(pcb_t));
    idle_task_pcb.pid = IDLE_TASK_PID;
    // Idle task runs entirely in kernel space, using the kernel's page directory
    idle_task_pcb.page_directory_phys = (uint32_t*)g_kernel_page_directory_phys;
    KERNEL_ASSERT(idle_task_pcb.page_directory_phys != NULL, "Kernel PD physical address is NULL during idle task init");
    idle_task_pcb.entry_point = (uintptr_t)kernel_idle_task_loop; // Function it will run
    idle_task_pcb.user_stack_top = NULL; // No user stack

    // --- Allocate Kernel Stack (using a static buffer for simplicity) ---
    // WARNING: In a more complex kernel, use a dynamic kernel stack allocator.
    // Ensure sufficient size for interrupt handling + context switch frame.
    static uint8_t idle_stack[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
    idle_task_pcb.kernel_stack_vaddr_top = (uint32_t*)((uintptr_t)idle_stack + sizeof(idle_stack));
    KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 512, "Idle task stack possibly too small");

    // --- Setup TCB ---
    memset(&idle_task_tcb, 0, sizeof(tcb_t));
    idle_task_tcb.process = &idle_task_pcb;
    idle_task_tcb.pid     = IDLE_TASK_PID;
    idle_task_tcb.state   = TASK_READY; // Idle task is always ready to run
    idle_task_tcb.has_run = true;       // Starts in kernel, considered "run"

    // --- Prepare Initial Stack Pointer for Idle Task's Kernel Context ---
    // The idle task starts in kernel mode. We set up the stack as if
    // context_switch saved its state just before calling kernel_idle_task_loop.
    uint32_t *kstack_ptr = (uint32_t *)idle_task_pcb.kernel_stack_vaddr_top;
    uintptr_t kstack_base = (uintptr_t)idle_stack;

    // Stack frame layout must match exactly what context_switch expects to POP.
    // Order (bottom of frame to top): DS, ES, FS, GS, EFLAGS, EDI, ESI, EBP, ESP_ignore, EBX, EDX, ECX, EAX, Return Address (EIP)

    *(--kstack_ptr) = (uint32_t)idle_task_pcb.entry_point; // "Return address" for context_switch's 'ret'
    *(--kstack_ptr) = 0; // EAX
    *(--kstack_ptr) = 0; // ECX
    *(--kstack_ptr) = 0; // EDX
    *(--kstack_ptr) = 0; // EBX
    *(--kstack_ptr) = 0; // ESP_ignore (value pushed by pushad, not used by popad)
    *(--kstack_ptr) = 0; // EBP (Initial base pointer can be 0)
    *(--kstack_ptr) = 0; // ESI
    *(--kstack_ptr) = 0; // EDI
    *(--kstack_ptr) = 0x00000202; // EFLAGS (IF=1, Reserved=1) - Interrupts enabled for idle task
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // DS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // ES
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // FS
    *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // GS

    // Sanity check stack pointer
    KERNEL_ASSERT((uintptr_t)kstack_ptr > kstack_base, "Idle task stack underflow during init");

    // Set the initial ESP for the idle task TCB
    idle_task_tcb.esp = kstack_ptr;

    SCHED_DEBUG("Idle task initialized: ESP=%p", idle_task_tcb.esp);
}

//----------------------------------------------------------------------------
// Public Scheduler API
//----------------------------------------------------------------------------

/**
 * @brief Initializes the scheduler subsystem.
 */
void scheduler_init(void)
{
    SCHED_LOG("Initializing scheduler...");
    task_list_head   = NULL;
    current_task     = NULL;
    task_count       = 0;
    context_switches = 0;
    g_scheduler_ready = false; // Initialize the global flag
    spinlock_init(&scheduler_lock);

    // Initialize and add the mandatory idle task
    scheduler_init_idle_task();
    task_list_head = &idle_task_tcb;
    idle_task_tcb.next = &idle_task_tcb; // Point to self
    task_count++;

    SCHED_LOG("Scheduler Initialized (Idle Task PID %lu created).", (unsigned long)IDLE_TASK_PID);
}

/**
 * @brief Creates a TCB for a given process and adds it to the scheduler's run queue.
 */
int scheduler_add_task(pcb_t *pcb)
{
    // --- Parameter Validation ---
    KERNEL_ASSERT(pcb != NULL, "NULL PCB passed");
    KERNEL_ASSERT(pcb->pid != IDLE_TASK_PID, "PID 0 is reserved for idle task");
    KERNEL_ASSERT(pcb->page_directory_phys != NULL, "PCB PD NULL");
    KERNEL_ASSERT(pcb->kernel_stack_vaddr_top != NULL, "PCB Kernel Stack NULL");
    KERNEL_ASSERT(pcb->user_stack_top != NULL, "PCB User Stack NULL");
    KERNEL_ASSERT(pcb->entry_point != 0, "PCB entry point is 0");
    KERNEL_ASSERT((uintptr_t)pcb->kernel_stack_vaddr_top > PROCESS_KSTACK_SIZE, "Kernel stack top invalid");
    KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 64, "Kernel stack too small for IRET"); // Ensure space for IRET frame

    SCHED_DEBUG("Adding task PID %lu Entry=%p UserStackTop=%p KernelStackTop=%p",
                (unsigned long)pcb->pid, (void*)pcb->entry_point,
                pcb->user_stack_top, pcb->kernel_stack_vaddr_top);

    // --- Allocate TCB ---
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) {
        SCHED_ERROR("kmalloc failed for TCB (PID %lu).", (unsigned long)pcb->pid);
        return SCHED_ERR_NOMEM;
    }
    memset(new_task, 0, sizeof(tcb_t));

    new_task->process = pcb;
    new_task->pid     = pcb->pid;
    new_task->state   = TASK_READY;
    new_task->has_run = false; // Mark as not yet run (important!)

    // --- Set up initial kernel stack frame JUST for the first IRET ---
    uint32_t *kstack_ptr = (uint32_t *)pcb->kernel_stack_vaddr_top;
    uintptr_t kstack_base = (uintptr_t)pcb->kernel_stack_vaddr_top - PROCESS_KSTACK_SIZE;

    SCHED_DEBUG("Preparing IRET stack frame for PID %lu at KStackTop=%p (Base=%p)",
                (unsigned long)pcb->pid, kstack_ptr, (void*)kstack_base);

    // --- IRET Frame (5 values pushed in reverse order) ---
    *(--kstack_ptr) = GDT_USER_DATA_SELECTOR | 3;  // User SS (0x23)
    *(--kstack_ptr) = (uintptr_t)pcb->user_stack_top; // User ESP (e.g., 0xC0000000)
    *(--kstack_ptr) = 0x00000202;                  // EFLAGS (IF=1, Reserved=1, IOPL=0)
    *(--kstack_ptr) = GDT_USER_CODE_SELECTOR | 3;  // User CS (0x1B)
    *(--kstack_ptr) = (uintptr_t)pcb->entry_point; // User EIP (e.g., 0x08048080)

    // Sanity check stack pointer didn't underflow
    KERNEL_ASSERT((uintptr_t)kstack_ptr > kstack_base, "Kernel stack underflow (IRET setup)");

    // Save the pointer to the top of the IRET frame. This is the ESP
    // that jump_to_user_mode will load before executing IRET.
    // Use the pcb field that prepare_initial_kernel_stack uses
    new_task->esp = (uint32_t*)pcb->kernel_esp_for_switch; // Use value set by prepare_initial_kernel_stack
    KERNEL_ASSERT(new_task->esp == kstack_ptr, "Mismatch between calculated ESP and PCB stored ESP");


    // --- Insert TCB into the circular linked list ---
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);
    KERNEL_ASSERT(task_list_head != NULL, "Task list head NULL (idle task missing?)");

    // Insert the new task after the head (which is initially the idle task)
    new_task->next = task_list_head->next;
    task_list_head->next = new_task;
    task_count++;

    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    SCHED_LOG("Added task PID %lu (KStackTop=%p, Init ESP for IRET=%p)",
              (unsigned long)pcb->pid,
              pcb->kernel_stack_vaddr_top,
              new_task->esp);

    return SCHED_OK; // success
}

/**
 * @brief Voluntarily yields the CPU to another task.
 */
void yield(void)
{
    // Disable interrupts before calling schedule
    uint32_t eflags = 0;
    asm volatile("pushf; pop %0; cli" : "=r"(eflags));

    schedule(); // Call the core scheduler

    // Restore interrupt state if they were enabled before yield
    if (eflags & 0x200) { // Check IF flag
        asm volatile("sti");
    }
}

/**
 * @brief Finds the next READY task to run (round-robin).
 * Skips the idle task unless it's the only task available or only other tasks are blocked/zombie.
 * @return Pointer to the next TCB to run. Never returns NULL if idle task exists.
 * @note Must be called with scheduler_lock held.
 */
static tcb_t* select_next_task(void)
{
    KERNEL_ASSERT(task_list_head != NULL, "select_next_task: task list is empty!");
    // If only the idle task exists, return it
    if (task_list_head->next == task_list_head) {
         KERNEL_ASSERT(task_list_head->pid == IDLE_TASK_PID, "Single task is not idle task?");
         KERNEL_ASSERT(task_list_head->state == TASK_READY, "Idle task not ready!");
         return task_list_head;
    }

    // Start search from the task *after* the current one.
    tcb_t *candidate = current_task ? current_task->next : task_list_head->next; // Start after current or after head
    tcb_t *starting_point = candidate;

    // Loop through the task list once looking for a non-idle READY task
    do {
        KERNEL_ASSERT(candidate != NULL, "NULL task encountered in scheduler list");

        if (candidate->pid != IDLE_TASK_PID && candidate->state == TASK_READY) {
            return candidate; // Found a ready user task
        }
        candidate = candidate->next;
    } while (candidate != starting_point);

    // If no regular task is ready, return the idle task
    // Find the idle task TCB (assuming it's always in the list)
    tcb_t* idle_task = task_list_head;
    while(idle_task->pid != IDLE_TASK_PID) {
        idle_task = idle_task->next;
        KERNEL_ASSERT(idle_task != task_list_head, "Idle task not found in list!"); // Should not happen
    }
    KERNEL_ASSERT(idle_task->state == TASK_READY, "Idle task is not in READY state!");
    return idle_task;
}

/**
 * @brief The core scheduler function. Selects next task and performs context switch.
 * @note Should be called with interrupts disabled.
 */
 void schedule(void)
{
    // Don't schedule if the scheduler hasn't been started yet
    if (!g_scheduler_ready) {
        return;
    }

    // Acquire lock, saving previous interrupt state
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    tcb_t *old_task = (tcb_t *)current_task; // Cast away volatile for local use
    tcb_t *new_task = select_next_task();    // Find the next task to run

    KERNEL_ASSERT(new_task != NULL, "select_next_task returned NULL!"); // Should always return at least idle

    // If the selected task is the same as the current one, no switch needed.
    if (new_task == old_task) {
        // Ensure the task is marked RUNNING if it was READY (e.g., after yield)
        if (current_task && current_task->state == TASK_READY) {
            current_task->state = TASK_RUNNING;
        }
        spinlock_release_irqrestore(&scheduler_lock, irq_flags);
        return;
    }

    // --- Prepare for Context Switch ---
    context_switches++;

    // Update state of the outgoing task (if any)
    if (old_task) {
        // If the task was running, mark it as ready for the next time.
        // If it was BLOCKED or ZOMBIE, leave its state as is.
        if (old_task->state == TASK_RUNNING) {
            old_task->state = TASK_READY;
        }
        // Sanity check the old task's state
        KERNEL_ASSERT(old_task->state == TASK_READY || old_task->state == TASK_BLOCKED || old_task->state == TASK_ZOMBIE,
                      "Old task has unexpected state during switch");
    }

    // Update state of the incoming task
    KERNEL_ASSERT(new_task->state == TASK_READY, "Selected next task was not in READY state?");
    new_task->state = TASK_RUNNING;
    current_task = new_task; // Update the global current task pointer

    // Get necessary info for the switch/jump
    if (new_task->process == NULL) {
         KERNEL_PANIC_HALT("New task process pointer is NULL!");
    }
    uint32_t *new_pd_phys = new_task->process->page_directory_phys;
    uint32_t *new_esp = new_task->esp; // This is the kernel stack pointer prepared for IRET/context_switch
    uint32_t **old_task_esp_loc = old_task ? &(old_task->esp) : NULL;
    bool first_run = !new_task->has_run; // Check if this is the task's first execution

    KERNEL_ASSERT(new_pd_phys != NULL, "New task page directory is NULL!");
    KERNEL_ASSERT(new_esp != NULL, "New task saved ESP is NULL!");

    // Mark the task as having run before releasing the lock and switching
    if (first_run) {
        new_task->has_run = true;
    }

    // --- Update TSS ESP0 ---
    // This MUST be done BEFORE switching to the new task, especially before the first jump to user mode.
    // It ensures that if an interrupt/syscall happens immediately in the new task,
    // the CPU knows where the correct kernel stack (esp0) is.
    // Do this for ALL switches to non-idle tasks.
    if (new_task->pid != IDLE_TASK_PID) {
         KERNEL_ASSERT(new_task->process != NULL && new_task->process->kernel_stack_vaddr_top != NULL,
                       "Switch target task has NULL process or kernel stack top");
         SCHED_DEBUG("Setting TSS ESP0 for switch to PID %lu: %p",
                     (unsigned long)new_task->pid, new_task->process->kernel_stack_vaddr_top);
         tss_set_kernel_stack((uint32_t)new_task->process->kernel_stack_vaddr_top);
    }

    // Release the scheduler lock *before* performing the context switch/jump
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    // --- Perform the switch ---
    if (first_run && new_task->pid != IDLE_TASK_PID) {
        // *** First time running a user task: jump using IRET ***
        SCHED_DEBUG("First run for PID %lu. Calling jump_to_user_mode(ESP=%p, PD=%p)",
                    (unsigned long)new_task->pid, new_esp, new_pd_phys);

        // Ensure interrupts are disabled before the ASM jump routine
        // (Though they should already be disabled from IRQ/Syscall entry)
        asm volatile("cli");

        // Now jump to user mode using the prepared kernel stack
        jump_to_user_mode(new_esp, new_pd_phys); // <<< Prototype visible now

        // jump_to_user_mode should NOT return. Panic if it does.
        KERNEL_PANIC_HALT("jump_to_user_mode returned unexpectedly!");

    } else {
        // *** Switching between kernel contexts (idle task or subsequent runs) ***
        // Determine if page directory needs to be switched
        bool pd_needs_switch = (!old_task || !old_task->process || old_task->process->page_directory_phys != new_pd_phys);

        SCHED_DEBUG("Context switch: %lu -> %lu (PD Switch: %s)",
                    old_task ? old_task->pid : 0,
                    new_task->pid,
                    pd_needs_switch ? "YES" : "NO");

        // TSS ESP0 was already updated above if needed for non-idle task.
        context_switch(old_task_esp_loc, new_esp, pd_needs_switch ? new_pd_phys : NULL); // <<< Prototype visible now
    }

    // --- Execution resumes here when THIS task gets switched back to ---
    // Interrupts will be enabled/disabled based on the EFLAGS restored by popfd/iret.
    // The scheduler lock is NOT held here.
}

/**
 * @brief Returns a volatile pointer to the currently running task's TCB.
 */
volatile tcb_t *get_current_task_volatile(void)
{
    // Read volatile global. Assumes read is atomic enough for target.
    // For SMP, this would typically return per-CPU data.
    return current_task;
}

/**
 * @brief Returns a non-volatile pointer to the currently running task's TCB.
 */
tcb_t *get_current_task(void)
{
    // Cast away volatile - caller must be careful about preemption.
    return (tcb_t *)current_task;
}

/**
 * @brief Marks the current running task as ZOMBIE and triggers a context switch.
 */
void remove_current_task_with_code(uint32_t code)
{
    // Ensure interrupts are disabled by caller or hardware, but cli for safety.
    asm volatile("cli");

    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    tcb_t *task_to_terminate = (tcb_t *)current_task;

    KERNEL_ASSERT(task_to_terminate != NULL, "remove_current_task called when current_task is NULL!");
    // Task might be READY or BLOCKED if termination signal handled between schedule points
    KERNEL_ASSERT(task_to_terminate->state == TASK_RUNNING || task_to_terminate->state == TASK_READY || task_to_terminate->state == TASK_BLOCKED,
                  "Task being removed is not RUNNING/READY/BLOCKED!");
    KERNEL_ASSERT(task_to_terminate->pid != IDLE_TASK_PID, "Attempting to remove the idle task!");

    SCHED_LOG("Task PID %lu exiting with code %lu. Marking as ZOMBIE.",
              (unsigned long)task_to_terminate->pid, (unsigned long)code);

    // Mark as ZOMBIE. It will be skipped by select_next_task and cleaned up later.
    task_to_terminate->state = TASK_ZOMBIE;
    // task_to_terminate->exit_code = code; // Store exit code if needed

    // Release lock *before* calling schedule
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);

    // Call schedule() to switch away from this ZOMBIE task.
    // Interrupts remain disabled until schedule() completes and the next task runs.
    schedule();

    // Should never return here.
    KERNEL_PANIC_HALT("Returned after schedule() in remove_current_task!");
}

/**
 * @brief Reap and free resources for any tasks marked as ZOMBIE.
 */
void scheduler_cleanup_zombies(void)
{
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);

    // Check if there are any tasks other than the idle task
    if (!task_list_head || task_count <= 1) {
        spinlock_release_irqrestore(&scheduler_lock, irq_flags);
        return; // Nothing to clean up besides idle task
    }

    tcb_t *prev = task_list_head;
    // Start checking from the task *after* the head (which might be idle)
    tcb_t *current = task_list_head->next;
    size_t checked_count = 0; // <<< Changed type to size_t
    size_t max_checks = task_count; // Maximum nodes to check in one pass

    // Iterate through the list, but max task_count times for safety
    while (current != task_list_head && checked_count < max_checks) { // <<< Comparison is now unsigned
        KERNEL_ASSERT(current != NULL, "NULL task in zombie cleanup loop!");
        // Skip the idle task itself (shouldn't be ZOMBIE anyway)
        if (current->pid == IDLE_TASK_PID) {
            prev = current;
            current = current->next;
            checked_count++;
            continue;
        }

        tcb_t *next_task = current->next; // Save next pointer before potential free

        if (current->state == TASK_ZOMBIE) {
            SCHED_LOG("Cleanup: Reaping ZOMBIE task PID %lu.", (unsigned long)current->pid);

            // --- Unlink the zombie task ---
            prev->next = next_task; // Link previous node to the node after current
            task_count--;
            KERNEL_ASSERT(task_count >= 1, "Task count fell below 1 during zombie cleanup");

            // If we removed the head's successor, and head might now point to itself
            // No special handling needed for head->next as prev->next covers it.

            // --- Resource Freeing ---
            pcb_t* pcb_to_free = current->process;
            tcb_t* tcb_to_free = current;

            // Drop the lock *before* calling potentially complex/blocking free functions
            spinlock_release_irqrestore(&scheduler_lock, irq_flags);

            // Free resources (outside the lock)
            if (pcb_to_free) {
                destroy_process(pcb_to_free); // Frees PCB, page dir, VMAs, etc.
            }
            kfree(tcb_to_free); // Frees the TCB itself

            // Reacquire lock to continue scan
            irq_flags = spinlock_acquire_irqsave(&scheduler_lock);
            // --- End Resource Freeing ---

            // Continue scan from the node *after* the removed one ('next_task')
            // 'prev' remains correct as it points to the node before 'next_task'
            current = next_task;
            // Don't increment checked_count here, as we adjusted the list

        } else {
            // Not a zombie, move pointers forward
            prev = current;
            current = next_task;
            checked_count++;
        }

         // Safety break if list seems corrupted
         if (current == NULL) {
              SCHED_ERROR("NULL pointer encountered during zombie cleanup scan!");
              break;
         }

         // Check secondary loop termination condition (removed redundant check)
         // if (checked_count > max_checks + 1) { // Allow one extra check <<< This check is redundant now
         //      SCHED_ERROR("Excessive checks during zombie cleanup scan!");
         //      break;
         // }

    } // End while loop

    spinlock_release_irqrestore(&scheduler_lock, irq_flags);
}


/**
 * @brief Retrieve basic scheduler statistics.
 */
void debug_scheduler_stats(uint32_t *out_task_count, uint32_t *out_switches)
{
    uintptr_t irq_flags = spinlock_acquire_irqsave(&scheduler_lock);
    if (out_task_count) {
        *out_task_count = task_count;
    }
    if (out_switches) {
        *out_switches = context_switches;
    }
    spinlock_release_irqrestore(&scheduler_lock, irq_flags);
}

/**
 * @brief Returns the ready state of the scheduler (for external checks).
 * Use scheduler_start() to actually enable scheduling.
 */
bool scheduler_is_ready(void)
{
    // Volatile read, no lock needed for single bool
    return g_scheduler_ready;
}

/**
 * @brief Marks the scheduler as ready to perform preemptive context switching.
 * Should be called after initialization and adding the first task(s), before enabling interrupts.
 */
void scheduler_start(void) {
    SCHED_LOG("Starting preemptive scheduling.");
    // Assuming interrupts are disabled by caller (usually kmain right before 'sti')
    g_scheduler_ready = true; // Set the global flag
}