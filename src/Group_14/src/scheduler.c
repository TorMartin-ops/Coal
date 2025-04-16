/**
 * scheduler.c - Improved Kernel Scheduler Implementation
 *
 * Features:
 *  - Task States: READY, RUNNING, BLOCKED, ZOMBIE for better management.
 *  - Locking: Uses spinlocks for SMP safety on scheduler data structures.
 *  - Robustness: Added checks for NULL pointers and invalid states.
 *  - Task Termination: Implements a safer termination mechanism (zombie state)
 *    deferring cleanup, avoiding context switch issues during removal.
 *  - Clarity: Improved structure, comments, and variable names.
 *  - Debugging: Includes assertions and targeted logging for critical paths.
 *
 * Required scheduler.h changes:
 *  - Add `task_state_t` enum definition (READY, RUNNING, BLOCKED, ZOMBIE).
 *  - Add `volatile task_state_t state;` and `uint32_t pid;` to `tcb_t`.
 */

 #include "terminal.h"   // For terminal_printf, terminal_write, etc.
 #include "scheduler.h"  // For tcb_t, pcb_t, function prototypes
 #include "process.h"    // For pcb_t, destroy_process
 #include "kmalloc.h"    // For kmalloc/kfree
 #include "string.h"     // For memset
 #include "types.h"      // Common integer types, e.g. uint32_t
 #include "spinlock.h"   // Spinlock implementation
 #include "idt.h"        // For interrupt enable/disable macros or placeholders
 
 // --- GDT Selectors (Ensure these match your GDT setup) ---
 #ifndef USER_CODE_SELECTOR
 #define USER_CODE_SELECTOR   0x1B  // GDT Index 3 + RPL 3
 #endif
 #ifndef USER_DATA_SELECTOR
 #define USER_DATA_SELECTOR   0x23  // GDT Index 4 + RPL 3
 #endif
 // Kernel segments (usually defined in GDT setup)
 #ifndef KERNEL_CODE_SELECTOR
 #define KERNEL_CODE_SELECTOR 0x08  // GDT Index 1
 #endif
 #ifndef KERNEL_DATA_SELECTOR
 #define KERNEL_DATA_SELECTOR 0x10  // GDT Index 2
 #endif
 
 // --- Kernel Assertion Macros (Define these if not available) ---
 #ifndef KERNEL_ASSERT
 #define KERNEL_ASSERT(condition, msg) do {                        \
     if (!(condition)) {                                           \
         terminal_printf("\n[SCHED ASSERT FAILED] %s at %s:%d\n",  \
             msg, __FILE__, __LINE__);                             \
         terminal_printf("System Halted.\n");                      \
         while (1) { asm volatile("cli; hlt"); }                   \
     }                                                             \
 } while (0)
 #endif
 
 #ifndef KERNEL_PANIC_HALT
 #define KERNEL_PANIC_HALT(msg) do {                               \
     terminal_printf("\n[SCHED PANIC] %s at %s:%d\n",              \
         msg, __FILE__, __LINE__);                                 \
     terminal_printf("System Halted.\n");                          \
     while (1) { asm volatile("cli; hlt"); }                       \
 } while (0)
 #endif
 
 // External assembly function for context switching.
 // Must match the assembly signature exactly.
 extern void context_switch(uint32_t **old_esp_ptr,
                            uint32_t *new_esp,
                            uint32_t *new_page_directory_phys);
 
 // --- Static Globals ---
 static tcb_t         *task_list_head   = NULL;    // Head of the circular list of ALL tasks
 static volatile tcb_t *current_task    = NULL;    // The currently RUNNING task (must be volatile)
 static uint32_t       task_count       = 0;       // Total tasks (any state)
 static uint32_t       context_switches = 0;       // Count how many times schedule() has switched
 static spinlock_t     scheduler_lock;             // Protects task list, current_task, counts
 
 /**
  * scheduler_init - Initialize the scheduler structures.
  * Call this once at kernel startup before adding tasks.
  */
 void scheduler_init(void)
 {
     task_list_head   = NULL;
     current_task     = NULL;
     task_count       = 0;
     context_switches = 0;
     spinlock_init(&scheduler_lock);
 
     terminal_write("[Scheduler] World-Class Scheduler Initialized.\n");
     // Optionally create/insert an idle task here.
 }
 
 /**
  * scheduler_add_task - Create a TCB around the given PCB and insert into the task list.
  * @pcb: Pointer to a valid pcb_t structure with valid page directory, stacks, entry point, etc.
  * Returns 0 on success, negative on error (e.g., no memory).
  */
 int scheduler_add_task(pcb_t *pcb)
 {
     // --- Parameter Validation ---
     KERNEL_ASSERT(pcb != NULL, "NULL PCB passed to scheduler_add_task");
     KERNEL_ASSERT(pcb->page_directory_phys != NULL, "PCB has NULL page directory");
     KERNEL_ASSERT(pcb->kernel_stack_vaddr_top != NULL, "PCB has NULL kernel stack top");
     KERNEL_ASSERT(pcb->user_stack_top != NULL, "PCB has NULL user stack top");
     // Check entry point
     KERNEL_ASSERT(pcb->entry_point != 0, "PCB has zero entry point");
 
     // If you have specific user-stack range checks, do them here:
     // Example (adjust these defines to match your system):
     //   #define USER_STACK_BOTTOM_VIRT 0x08000000
     //   #define USER_STACK_TOP_VIRT_ADDR 0x08010000
     //   #define PROCESS_KSTACK_SIZE 4096
     // Adjust to your real constants:
     // KERNEL_ASSERT((uintptr_t)pcb->user_stack_top > USER_STACK_BOTTOM_VIRT &&
     //               (uintptr_t)pcb->user_stack_top <= USER_STACK_TOP_VIRT_ADDR,
     //               "PCB user_stack_top is outside expected range");
 
     terminal_printf("[SchedAddTask Debug] PID %d Entry=0x%x UserStackTop=0x%x KernelStackTop=0x%x\n",
                     pcb->pid, pcb->entry_point,
                     (uintptr_t)pcb->user_stack_top,
                     (uintptr_t)pcb->kernel_stack_vaddr_top);
 
     // --- Allocate TCB ---
     tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
     if (!new_task) {
         terminal_write("[Scheduler] Error: Out of memory for TCB.\n");
         return -2; // Out-of-memory error code
     }
     memset(new_task, 0, sizeof(tcb_t));
 
     new_task->process = pcb;
     new_task->pid     = pcb->pid;       // Copy PID for logging convenience
     new_task->state   = TASK_READY;     // Start in READY state
 
     // --- Set up initial kernel stack frame for user mode iret ---
     uint32_t *kstack_ptr = (uint32_t *)pcb->kernel_stack_vaddr_top;
 
     // Ensure minimal stack space is available for the context-switch frame.
     // (5 * 4 bytes for iret) + (8 * 4 bytes for pushad) + (4 * 4 for segment regs) + 4 for eflags
     // = 20 + 32 + 16 + 4 = 72 bytes. Check your process's kernel stack is large enough.
     // Adjust to your real define (PROCESS_KSTACK_SIZE). For example:
     // KERNEL_ASSERT(PROCESS_KSTACK_SIZE >= 128, "Kernel stack size too small for initial frame");
 
     // iret frame: (pushed last --> first on the stack)
     *(--kstack_ptr) = USER_DATA_SELECTOR;            // 5. user DS/SS
     *(--kstack_ptr) = (uint32_t)pcb->user_stack_top; // 4. user ESP
     *(--kstack_ptr) = 0x00000202;                    // 3. EFLAGS (IF=1, IOPL=0)
     *(--kstack_ptr) = USER_CODE_SELECTOR;            // 2. user CS
     *(--kstack_ptr) = (uint32_t)pcb->entry_point;    // 1. user EIP
 
     // Stack frame for context_switch restoration (matching the pops in context_switch assembly):
     *(--kstack_ptr) = 0; // EAX
     *(--kstack_ptr) = 0; // ECX
     *(--kstack_ptr) = 0; // EDX
     *(--kstack_ptr) = 0; // EBX
     *(--kstack_ptr) = 0; // ESP_ignore (popad does not restore ESP)
     *(--kstack_ptr) = 0; // EBP
     *(--kstack_ptr) = 0; // ESI
     *(--kstack_ptr) = 0; // EDI
     *(--kstack_ptr) = 0x00000202;         // EFLAGS (pushed by pushfd in context_switch)
     *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // DS
     *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // ES
     *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // FS
     *(--kstack_ptr) = KERNEL_DATA_SELECTOR; // GS
 
     // Save final kernel stack pointer into TCB.
     new_task->esp = kstack_ptr;
 
     // --- Insert TCB into the circular linked list (protected by spinlock) ---
     uintptr_t flags = spinlock_acquire_irqsave(&scheduler_lock);
     if (task_list_head == NULL) {
         // First task in the system
         task_list_head  = new_task;
         new_task->next  = new_task; // Circular: points back to itself
     } else {
         // Find the tail (the node whose next is head)
         tcb_t *tail = task_list_head;
         while (tail->next != task_list_head) {
             tail = tail->next;
         }
         tail->next      = new_task;
         new_task->next  = task_list_head;
     }
     task_count++;
     spinlock_release_irqrestore(&scheduler_lock, flags);
 
     terminal_printf("[Scheduler] Added task PID %d (KStackTop=0x%x, Init ESP=0x%x)\n",
                     pcb->pid,
                     (uintptr_t)pcb->kernel_stack_vaddr_top,
                     (uintptr_t)new_task->esp);
 
     return 0; // success
 }
 
 /**
  * yield - Voluntarily give up the CPU and let scheduler pick another task.
  * Typically called from a system call or a user-level function that requests a yield.
  * If interrupts are enabled, `schedule()` can be called directly or via an interrupt handler.
  */
 void yield(void)
 {
     // For a simple approach, call schedule() directly.
     // Make sure interrupts are disabled inside schedule() if needed.
     schedule();
 }
 
 /**
  * select_next_task - Finds the next READY task to run (circularly).
  * Returns NULL if no suitable READY task is found.
  * Called with scheduler_lock held (interrupts disabled).
  */
 static tcb_t* select_next_task(void)
 {
     if (!task_list_head) {
         return NULL; // No tasks at all
     }
 
     // Start from the task after the current task, or head if current is NULL
     tcb_t *candidate      = current_task ? current_task->next : task_list_head;
     tcb_t *starting_point = candidate;
 
     do {
         if (candidate && candidate->state == TASK_READY) {
             return candidate; // Found a ready task
         }
         // If we detect corruption or an unexpected NULL in the circular list, break
         if (!candidate) break;
         candidate = candidate->next;
     } while (candidate != starting_point);
 
     // No other ready tasks found. Check if the current task is itself ready.
     if (current_task && current_task->state == TASK_READY) {
         return (tcb_t *)current_task; // (Cast away volatile)
     }
 
     // Otherwise, no tasks are ready -> return NULL so we can idle
     return NULL;
 }
 
 /**
  * schedule - The core scheduler function. Finds a new READY task to run and context-switches.
  * Should be called with interrupts disabled (or from an interrupt handler).
  */
 void schedule(void)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&scheduler_lock);
     context_switches++;
 
     tcb_t *old_task = (tcb_t *)current_task;  // Cast away volatile for local use
     tcb_t *new_task = select_next_task();
 
     // --- Idle Task Handling ---
     if (new_task == NULL) {
         // No ready tasks exist. If we have a currently running task, we might want to mark it READY.
         // Or, if you're implementing an actual "idle" TCB, you could pick that here.
         if (old_task && old_task->state == TASK_RUNNING) {
             old_task->state = TASK_READY;
         }
         // Release lock and return so that the caller can do a 'hlt' or similar.
         spinlock_release_irqrestore(&scheduler_lock, flags);
         return;
     }
 
     // If the next task is the same as the old task, no switch needed.
     if (new_task == old_task) {
         // Ensure the current (old) task is marked RUNNING if it wasn't
         if (current_task && current_task->state != TASK_RUNNING) {
             current_task->state = TASK_RUNNING;
         }
         spinlock_release_irqrestore(&scheduler_lock, flags);
         return;
     }
 
     // Mark the old task as READY if it's still RUNNING (and not a ZOMBIE/BLOCKED)
     if (old_task) {
         KERNEL_ASSERT(old_task->state == TASK_RUNNING,
             "Old task wasn't RUNNING during switch?");
         if (old_task->state != TASK_ZOMBIE) {
             old_task->state = TASK_READY;
         }
     }
 
     // Mark new task as RUNNING
     KERNEL_ASSERT(new_task->state == TASK_READY, "Selected next task not READY?");
     new_task->state = TASK_RUNNING;
     current_task    = new_task; // Update global pointer
 
     // Potentially switch address spaces if the tasks belong to different processes
     uint32_t *new_pd_phys = NULL;
     if (!old_task || old_task->process != new_task->process) {
         KERNEL_ASSERT(new_task->process != NULL,
             "New task has NULL process pointer!");
         KERNEL_ASSERT(new_task->process->page_directory_phys != NULL,
             "New task has NULL page directory!");
         new_pd_phys = new_task->process->page_directory_phys;
     }
 
     // The old task’s ESP field must be saved by the context_switch
     // We pass the address of old_task->esp if old_task != NULL
     uint32_t **old_task_esp_loc = old_task ? &(old_task->esp) : NULL;
 
     // Release lock before switching
     spinlock_release_irqrestore(&scheduler_lock, flags);
 
     // Context Switch: This does not return here until we switch back
     context_switch(old_task_esp_loc, new_task->esp, new_pd_phys);
 
     // --- Execution resumes here when we switch back to old_task in the future ---
     // We do not re-acquire the lock automatically. The CPU state is restored from old_task's stack.
 }
 
 /**
  * get_current_task - Returns a pointer to the currently running task's TCB (or NULL if none).
  * Typically used by system calls to find the calling process.
  */
 tcb_t *get_current_task(void)
 {
     // Simple read of a volatile pointer. If you have SMP with multiple cores,
     // you might need to be more careful (e.g., use the current CPU's current_task).
     return (tcb_t *)current_task;
 }
 
 /**
  * remove_current_task_with_code - Marks the current running task as ZOMBIE, then calls schedule().
  * The actual cleanup is deferred to scheduler_cleanup_zombies().
  * @code: Exit code or reason for removal, purely for logging in this example.
  */
 void remove_current_task_with_code(uint32_t code)
 {
     // Disable interrupts if not already
     asm volatile("cli");
 
     uintptr_t flags = spinlock_acquire_irqsave(&scheduler_lock);
 
     tcb_t *task_to_terminate = (tcb_t *)current_task; // cast away volatile
 
     KERNEL_ASSERT(task_to_terminate != NULL,
         "remove_current_task called when current_task is NULL!");
     KERNEL_ASSERT(task_to_terminate->state == TASK_RUNNING,
         "Task being removed is not RUNNING!");
 
     terminal_printf("[Scheduler] Task PID %d exiting with code %d. Marking as ZOMBIE.\n",
                     task_to_terminate->pid, code);
 
     // Mark the task as ZOMBIE
     task_to_terminate->state = TASK_ZOMBIE;
 
     // Release lock before scheduling
     spinlock_release_irqrestore(&scheduler_lock, flags);
 
     // Call schedule() to switch to a different task.
     schedule();
 
     // We should never come back here if the scheduler picks a different task,
     // because context_switch won't return to this stack again.
     KERNEL_PANIC_HALT("Returned after schedule() in remove_current_task!");
 }
 
 /**
  * scheduler_cleanup_zombies - Reap and free any tasks marked as ZOMBIE.
  * Should be called occasionally (e.g., from idle task or a dedicated "reaper" task).
  */
 void scheduler_cleanup_zombies(void)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&scheduler_lock);
 
     if (!task_list_head) {
         spinlock_release_irqrestore(&scheduler_lock, flags);
         return; // No tasks to clean
     }
 
     // We'll walk the circular list, checking each TCB for ZOMBIE state.
     // We'll remove it from the list and free it, including the associated PCB.
 
     tcb_t *prev = task_list_head;
     // Find the tail (the node whose ->next == head) to start cleanly
     while (prev->next != task_list_head) {
         prev = prev->next;
     }
     // Now prev is tail. We'll iterate from head around.
 
     tcb_t *current = task_list_head;
     int tasks_processed = 0;
 
     while (task_count > 0 && tasks_processed <= (int)task_count + 1)
     {
         KERNEL_ASSERT(current != NULL, "NULL task in zombie cleanup loop!");
         tcb_t *next_task = current->next;
 
         if (current->state == TASK_ZOMBIE) {
             terminal_printf("[Scheduler Cleanup] Reaping ZOMBIE task PID %d.\n",
                             current->pid);
 
             // Unlink from circular list
             prev->next = next_task;
             task_count--;
 
             // If we're removing the head, update task_list_head
             if (task_list_head == current) {
                 task_list_head = (task_count == 0) ? NULL : next_task;
             }
 
             // Drop the lock before actually freeing
             spinlock_release_irqrestore(&scheduler_lock, flags);
 
             // Free resources
             if (current->process) {
                 destroy_process(current->process); // Freed the PCB, page dir, etc.
             }
             kfree(current);  // Freed the TCB
 
             // Reacquire lock to continue
             flags = spinlock_acquire_irqsave(&scheduler_lock);
 
             // If list is empty now, we're done
             if (!task_list_head) {
                 break;
             }
 
             // Reset scanning if we removed the head
             if (task_list_head == next_task) {
                 // Re-find the tail if needed
                 tcb_t *tmp = task_list_head;
                 while (tmp->next != task_list_head) {
                     tmp = tmp->next;
                 }
                 prev = tmp;
                 current = task_list_head;
             } else {
                 // Otherwise, prev stays the same, we just continue from next_task
                 current = next_task;
             }
         } else {
             // Not a zombie, move forward
             prev = current;
             current = next_task;
         }
 
         tasks_processed++;
         if (!task_list_head) break;
         // If we've looped around to the head again, break
         if (current == task_list_head) {
             break;
         }
     }
 
     spinlock_release_irqrestore(&scheduler_lock, flags);
 }
 
 /**
  * debug_scheduler_stats - Retrieve or print basic scheduler stats.
  * @out_task_count: If non-NULL, will store the current number of tasks.
  * @out_switches:   If non-NULL, will store how many context switches have occurred.
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
 